/*
 * Device and display-mode enumeration, rendered as the JSON the settings
 * window consumes.
 *
 * This lives apart from main.cpp because the plugin needs it by two different
 * routes.  With no output running it invokes --list-devices / --list-modes as
 * a one-shot subprocess.  With output running it cannot: IINA's utils.exec
 * serialises, and the long-lived helper process is itself an outstanding exec,
 * so a second one never resolves.  The attached-mode session therefore answers
 * the same queries over the control socket, and both paths must produce
 * identical JSON.
 */

#ifndef IINA_DECKLINK_ENUMERATE_H
#define IINA_DECKLINK_ENUMERATE_H

#include <string>

// {"driver":bool,"devices":[name,...]}
//
// An empty list means the driver is installed but nothing is connected; the
// "driver" flag distinguishes that from Desktop Video being absent entirely,
// which is the case worth telling the user about.
std::string devices_json();

// {"modes":[{"code","width","height","fps","progressive"},...]}
// An empty device name means the first device found.
std::string modes_json(const std::string &device);

#endif  // IINA_DECKLINK_ENUMERATE_H
