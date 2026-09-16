// Uninstall support. The installer registers OMTMini.exe --uninstall as the
// UninstallString so there is no second binary to keep around.
#pragma once
#include <string>

namespace uninstall {
// Returns true when the command line asked for an uninstall.
bool requested();
bool silent();
// Removes shortcuts, registry entries and the virtual camera registration,
// then schedules the program folder for deletion once this process exits.
void run();
}
