#pragma once
#include <Arduino.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  fb - minimal Firebase Realtime Database client over the REST API.
 *
 *  Authenticates as a dedicated Firebase Auth user (email + password from
 *  config.h), exchanges that for an ID token, and refreshes the token before
 *  it expires. The database rules confine that user's writes to this device's
 *  own subtree, so a device that is stolen or dumped cannot touch anything
 *  else in the project.
 *
 *  Every call blocks on TLS, so this is only ever used from the monitor task,
 *  never from loop() or an async web handler.
 * ─────────────────────────────────────────────────────────────────────────── */
namespace fb {

void begin();

/** Sign in, or refresh a token that is about to expire. Cheap when neither is
 *  needed. Returns whether a usable credential is now held. A no-op when a
 *  legacy database secret is configured. */
bool ensureAuth();
bool ready();

/** True when FIREBASE_DB_SECRET is set and the token path is bypassed. */
bool usingSecret();

/** PUT a JSON document at `path` (no leading slash, no .json suffix). */
bool put(const char *path, const String &json);

const char *lastError();
uint32_t    tokenExpiresInS();
uint32_t    consecutiveFailures();

}  // namespace fb
