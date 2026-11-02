// The help topics of the remake's additions (options, buttons), in the
// format of `help.txt`, shown as the original's help is.
#pragma once

#include <string_view>

#include "ui/winmgr.h"

namespace oa {

// Shows the remake's help topic `topic` in the help window, run modally
// (as `WinMgr` does for the `help.txt` topics). Throws `DataError` without
// the HELPWINDOW.
void show_remake_help(WinMgr& wm, std::string_view topic);

class HelpWnd;
// Loads the remake's help topic `topic` into `help`, its texts in the
// language played (without showing it).
void open_remake_topic(WinMgr& wm, HelpWnd& help, std::string_view topic);

}  // namespace oa
