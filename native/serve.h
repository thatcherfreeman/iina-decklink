/*
 * Attached mode: connect back to the plugin's WebSocket server and run until
 * told to stop, the connection drops, or the watchdog fires.
 */

#ifndef IINA_DECKLINK_SERVE_H
#define IINA_DECKLINK_SERVE_H

#include <string>

#include "player.h"

// `defaults` seeds the output configuration; the plugin overrides it with a
// "configure" message before the first "load".
int run_serve(const std::string &url, const OutputConfig &defaults);

#endif  // IINA_DECKLINK_SERVE_H
