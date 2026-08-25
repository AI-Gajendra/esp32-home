#pragma once

/* FW_VERSION comes from platformio.ini (build_flags). Bump it there before
 * every OTA so the /update page tells you which build is actually running. */
#ifndef FW_VERSION
  #define FW_VERSION "0.0.0-dev"
#endif

#define FW_BUILD_DATE __DATE__ " " __TIME__
