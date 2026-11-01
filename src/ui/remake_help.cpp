#include "ui/remake_help.h"

#include <cstdint>
#include <span>
#include <string_view>

#include "core/error.h"
#include "ui/help_wnd.h"

namespace oa {

namespace {

// The help's mentions of the keyboard shortcuts (none on Android).
#ifdef __ANDROID__
#define OA_KEY_HINT(text) ""
#else
#define OA_KEY_HINT(text) " " text
#endif

// The remake's help topics, in the format of `help.txt`.
constexpr std::string_view kHelpFile = "OpenAscendance help";
constexpr std::string_view kHelpText = R"(// The help topics of the remake's additions.

help oa_options
text
Options
endtext
pos 0 15
flags TJ_HC
text
Sets the game variant of new games, the display and the sound.
endtext
pos 5 50
button
title OK
msg 0

help oa_screen
text
Options
endtext
pos 0 15
flags TJ_HC
text
Each setting applies as soon as it is picked. OK keeps them, Cancel puts back those in force when this screen was opened. Shift and a click on a row tells more about
it.)" OA_KEY_HINT("Enter and Esc keep them too.") R"(
endtext
pos 5 50
button
title OK
msg 0

help oa_variant
text
Game Variant
endtext
pos 0 15
flags TJ_HC
text
The rules and opponents of the next new game. Standard: those of Ascendancy version 1.6.5. Antagonizer: those of the Antagonizer edition, whose opponents play harder,
with a few rule changes. peCustom: the Antagonizer's, as this fork develops them further. A game keeps its variant in its saves.
endtext
pos 5 50
button
title OK
msg 0

help oa_mood
text
Mood
endtext
pos 0 15
flags TJ_HC
text
How this species feels about you: the mark shows its mood, the zones what it does there.
@Red: declares war when it has a reason and expects to win.
@Orange: refuses peace.
@Yellow: accepts peace.
@Grey-green: offers peace, but breaks an alliance.
@Light green: keeps an alliance.
@Green: accepts an alliance.
@Bright green: offers an alliance.
endtext
pos 5 50
button
title OK
msg 0

help oa_display
text
Display
endtext
pos 0 15
flags TJ_HC
text
Plays in a window or on the full screen.)" OA_KEY_HINT("Alt+Enter also switches between them.") R"(
endtext
pos 5 50
button
title OK
msg 0

help oa_scaling
text
Scaling
endtext
pos 0 15
flags TJ_HC
text
How the picture is enlarged to the window. Smooth blends the pixels, Sharp keeps them square as on the original screen.
endtext
pos 5 50
button
title OK
msg 0

help oa_music
text
Music
endtext
pos 0 15
flags TJ_HC
text
The music's volume: a click, a tap or a drag on the bar sets it, at the far left no music. At 100 % it plays as loud as in the
original.)" OA_KEY_HINT("The wheel and the arrow keys change it by steps; Alt+M switches the music off and back on at this volume.") R"(
endtext
pos 5 50
button
title OK
msg 0

help oa_sound
text
Sound Effects
endtext
pos 0 15
flags TJ_HC
text
The sound effects' volume, played at each change: a click, a tap or a drag on the bar sets it, at the far left no sound effects. At 100 % they play as loud as in the
original, louder than the music.)" OA_KEY_HINT("The wheel and the arrow keys change it by steps; Alt+N switches them off and back on at this volume.") R"(
endtext
pos 5 50
button
title OK
msg 0

help oa_language
text
Language
endtext
pos 0 15
flags TJ_HC
text
The language of the game's texts, from the next start of the program. Original: the texts of the game files as installed. Saves load in either language.
endtext
pos 5 50
button
title OK
msg 0

help oa_ok
text
OK
endtext
pos 0 15
flags TJ_HC
text
Keeps these settings and goes back to the opening menu. A new game variant applies to the next new game, a new language from the next
start.)" OA_KEY_HINT("Enter and Esc do the same.") R"(
endtext
pos 5 50
button
title OK
msg 0

help oa_cancel
text
Cancel
endtext
pos 0 15
flags TJ_HC
text
Puts back the settings in force when this screen was opened and goes back to the opening menu.
endtext
pos 5 50
button
title OK
msg 0

help oa_deselect
text
Deselect Item
endtext
pos 0 15
flags TJ_HC
text
This button is used to deselect the currently selected item, cancelling its move or target.
endtext
pos 5 50
button
title OK
msg 0

help end
)";
#undef OA_KEY_HINT

}  // namespace

void show_remake_help(WinMgr& wm, std::string_view topic) {
  auto* help = dynamic_cast<HelpWnd*>(wm.find_window("HELPWINDOW"));
  if (help == nullptr) {
    throw DataError("windows.txt: no HELPWINDOW");
  }
  help->open_topic(kHelpFile, std::span(reinterpret_cast<const std::uint8_t*>(kHelpText.data()), kHelpText.size()),
                   topic);
  HelpTopic& content = help->content();
  for (HelpElement& element : content.elements) {
    // The parser ends a text with its last line end, a space, and joins its
    // lines with one; the translation table's English texts are the lines
    // joined, without the end.
    std::string_view english = element.text;
    if (english.ends_with(' ')) {
      english.remove_suffix(1);
    }
    element.text = wm.remake_text(english);
  }
  for (HelpButton& button : content.buttons) {
    if (button.title) {
      button.title = wm.remake_text(*button.title);
    }
  }
  if (!wm.in_modal()) {
    wm.do_modal(*help, true);
  }
}

}  // namespace oa
