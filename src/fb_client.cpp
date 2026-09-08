#include "fb_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "certs.h"
#include "config.h"

namespace fb {
namespace {

constexpr uint32_t REFRESH_MARGIN_S = 300;   /* renew 5 min before expiry */
constexpr uint16_t HTTP_TIMEOUT_MS  = 12000;

String   s_idToken;
String   s_refreshToken;
String   s_uid;
uint32_t s_expiresAtMs = 0;
uint32_t s_failures     = 0;
uint32_t s_nextAttemptMs = 0;
char     s_error[96]    = "";

constexpr uint32_t BACKOFF_MIN_MS = 30UL * 1000;
constexpr uint32_t BACKOFF_MAX_MS = 10UL * 60 * 1000;

void setError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

/* One TLS request. The client is a local so its ~40 KB of buffers are handed
 * back as soon as the call returns - the device has other work to do with that
 * heap, and a long-lived secure client would hold it for ever. */
int request(const char *url, const char *contentType, const String &body,
            String &out, const char *method = "POST") {
    WiFiClientSecure client;
    client.setCACert(GOOGLE_ROOT_CA);
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, url)) {
        setError("begin() failed");
        return -1;
    }
    if (contentType) http.addHeader("Content-Type", contentType);

    int code = (strcmp(method, "PUT") == 0) ? http.PUT(body) : http.POST(body);
    out = (code > 0) ? http.getString() : String();
    http.end();

    if (code <= 0) setError("transport: %s", HTTPClient::errorToString(code).c_str());
    return code;
}

/* Pull only the handful of fields we need out of a multi-kilobyte auth reply,
 * so the JWT is the only large string that ever lands in RAM. */
bool parseAuth(const String &body, const char *idKey, const char *refreshKey,
               const char *expiresKey) {
    JsonDocument filter;
    filter[idKey]      = true;
    filter[refreshKey] = true;
    filter[expiresKey] = true;
    filter["localId"]  = true;
    filter["user_id"]  = true;
    filter["error"]["message"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        setError("json: %s", err.c_str());
        return false;
    }
    if (doc["error"]["message"].is<const char *>()) {
        setError("%s", doc["error"]["message"].as<const char *>());
        return false;
    }

    const char *id = doc[idKey];
    const char *rt = doc[refreshKey];
    if (!id || !rt) {
        setError("auth reply missing token");
        return false;
    }

    s_idToken      = id;
    s_refreshToken = rt;

    if (doc["localId"].is<const char *>())      s_uid = doc["localId"].as<const char *>();
    else if (doc["user_id"].is<const char *>()) s_uid = doc["user_id"].as<const char *>();

    uint32_t ttl = 3600;
    if (doc[expiresKey].is<const char *>())  ttl = atol(doc[expiresKey].as<const char *>());
    else if (doc[expiresKey].is<uint32_t>()) ttl = doc[expiresKey].as<uint32_t>();
    if (ttl < 60) ttl = 60;

    s_expiresAtMs = millis() + ttl * 1000;
    return true;
}

bool signIn() {
    String url = String("https://identitytoolkit.googleapis.com/v1/"
                        "accounts:signInWithPassword?key=") + FIREBASE_API_KEY;

    JsonDocument req;
    req["email"]             = FIREBASE_EMAIL;
    req["password"]          = FIREBASE_PASSWORD;
    req["returnSecureToken"] = true;
    String body;
    serializeJson(req, body);

    String resp;
    int code = request(url.c_str(), "application/json", body, resp);
    if (code != 200) {
        if (code > 0) parseAuth(resp, "idToken", "refreshToken", "expiresIn");
        setError("signIn HTTP %d: %s", code, s_error);
        return false;
    }
    if (!parseAuth(resp, "idToken", "refreshToken", "expiresIn")) return false;

    Serial.printf("[fb] signed in as %s (uid %s)\n", FIREBASE_EMAIL, s_uid.c_str());
    return true;
}

bool refresh() {
    String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
    String body = "grant_type=refresh_token&refresh_token=" + s_refreshToken;

    String resp;
    int code = request(url.c_str(), "application/x-www-form-urlencoded", body, resp);
    if (code != 200) {
        setError("refresh HTTP %d", code);
        return false;
    }
    if (!parseAuth(resp, "id_token", "refresh_token", "expires_in")) return false;

    Serial.println("[fb] token refreshed");
    return true;
}

}  // namespace

void begin() {
    s_idToken = "";
    s_refreshToken = "";
    s_expiresAtMs = 0;
    s_failures = 0;
    s_nextAttemptMs = 0;
    setError("not signed in yet");
}

bool ready() {
    return s_idToken.length() > 0 && (int32_t)(s_expiresAtMs - millis()) > 0;
}

uint32_t tokenExpiresInS() {
    if (!s_idToken.length()) return 0;
    int32_t left = (int32_t)(s_expiresAtMs - millis());
    return left > 0 ? (uint32_t)(left / 1000) : 0;
}

uint32_t consecutiveFailures() { return s_failures; }

bool ensureAuth() {
    if (ready() && tokenExpiresInS() > REFRESH_MARGIN_S) return true;

    /* Back off after repeated failures. Until the Firebase user exists, every
     * attempt is a wasted TLS handshake once a minute; the other sink is
     * carrying the data meanwhile, so there is no hurry. */
    if (s_failures > 0 && (int32_t)(millis() - s_nextAttemptMs) < 0) {
        setError("auth backoff, %lus left",
                 (unsigned long)((s_nextAttemptMs - millis()) / 1000));
        return false;
    }

    bool ok = false;
    if (s_refreshToken.length()) {
        ok = refresh();
        /* A refresh token can be revoked or the account disabled; falling back
         * to a full sign-in makes that self-healing rather than terminal. */
        if (!ok) {
            Serial.printf("[fb] refresh failed (%s) - signing in again\n", s_error);
            s_refreshToken = "";
        }
    }
    if (!ok) ok = signIn();

    if (ok) {
        s_failures = 0;
    } else {
        s_failures++;
        uint32_t wait = BACKOFF_MIN_MS << (s_failures > 5 ? 5 : s_failures - 1);
        if (wait > BACKOFF_MAX_MS) wait = BACKOFF_MAX_MS;
        s_nextAttemptMs = millis() + wait;
        Serial.printf("[fb] auth failed (%s) - next try in %lus\n",
                      s_error, (unsigned long)(wait / 1000));
    }
    return ok;
}

bool put(const char *path, const String &json) {
    if (!ensureAuth()) return false;

    String url = String(FIREBASE_DB_URL) + "/" + path + ".json?auth=" + s_idToken;
    String resp;
    int code = request(url.c_str(), "application/json", json, resp, "PUT");

    if (code != 200) {
        setError("PUT %s -> HTTP %d", path, code);
        s_failures++;
        /* 401 means the token went stale early; drop it so the next attempt
         * re-authenticates instead of retrying with a dead credential. */
        if (code == 401) s_expiresAtMs = millis();
        return false;
    }
    s_failures = 0;
    return true;
}

const char *lastError() { return s_error; }

}  // namespace fb
