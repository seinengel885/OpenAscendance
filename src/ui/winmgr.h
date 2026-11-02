// The window manager (`winmgr.cpp`, global `WinMgr` 0xfa344): the windows of
// `windows.txt`, the screens listing them, event delivery, the system message
// queue, window timers and input notifications, the screen history, modal
// loops, help mode, the exit corners, the hotkeys and the per-pass input
// handling and refresh of the main loop (`docs/07-ui-framework.md`, "Window
// manager").
//
// Also the game events of the star map (`WinMgr_show_game_events` 0x50f84)
// and the replay of window-event traces (the tutorials, `docs/07-ui-framework.md`
// "Recording and tutorials"). Not here: the developer keys and the
// recorder.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assets/cfile.h"
#include "assets/windows_txt.h"
#include "assets/winevent.h"
#include "audio/sound_system.h"
#include "game/events.h"
#include "gfx/font.h"
#include "gfx/gsystem.h"
#include "platform/input.h"
#include "ui/game_session.h"
#include "ui/gwindow.h"
#include "ui/shape_cache.h"

namespace oa {

// System messages (`WinMgr_dispatch_messages` 0x51290).
enum SystemMessage : int {
  kMessageNone = 0,
  kMessageChangeScreen = 1,  // p1 screen, p2 != 0 pushes the current one
  kMessageQuit = 2,
  kMessageBack = 3,
  kMessageReplay = 4,    // replay WINEVENT.BIN (developer)
  kMessageHelp = 5,      // text topic, p2 a `HelpKind`
  kMessageEndModal = 6,  // p1 the modal result
  kMessageNextTurn = 7,  // the next day (NEXTTURN)
  kMessageAutoTurn = 8,  // next days until an event (NEXTTURNCONT)
};

// The kinds of help of message 5 (its p2).
enum HelpKind : int {
  kHelpTopic = 0,     // help.txt topic `text`
  kHelpAbility = 1,   // the special abilities window
  kHelpNumbered = 2,  // help.txt topic `help%d` of p1
  kHelpNoIntro = 3,   // help.txt topic NOINTRO
  kHelpGizmo = 4,     // gizhelp.txt
  kHelpPlanet = 5,    // planhelp.txt
};

// A pointer position, in screen pixels.
struct PointerPos {
  int x = 0;
  int y = 0;

  bool operator==(const PointerPos&) const = default;
};

// The pointer frames of `MOUSE.SHP` (`GSystem_set_pointer`).
enum PointerFrame : int { kPointerNormal = 0, kPointerHelp = 1, kPointerExitCorner = 4 };

// What the window manager and the windows use of the rest of the program.
struct UiServices {
  const GameFiles& files;
  GSystem& gsystem;
  SoundSystem& sound;
  GameSession& game;
  // The writable folder of `ascend.cfg` and the Alt+G screenshots (the
  // original's current directory); empty for none (nothing is written).
  std::filesystem::path user_dir;
  // Microseconds of the 100 Hz timer's clock (the FLIC timer's base).
  std::function<std::uint64_t()> now_us;
  // The 100 Hz tick counter (`0x12202c`).
  std::function<std::uint32_t()> ticks;
  // Waits about a millisecond (the main loop's 3-tick wait).
  std::function<void()> idle;
  // Shows or hides the software mouse pointer (`MOUSE_show` / `MOUSE_hide`).
  std::function<void(bool)> show_pointer;
  // Sets the pointer's `MOUSE.SHP` frame (`GSystem_set_pointer`).
  std::function<void(int)> set_pointer_frame;
  // `input_poll` (0x25c8c): the input of the next pass.
  std::function<const InputState&()> poll_input;
  // Shows the frame at the end of a pass (the dirty panes' copy to video
  // memory).
  std::function<void()> present;
  // The picture on the screen: the last frame shown, with the mouse pointer
  // (what the VFX driver reads back for the Alt+G screenshots); none: the
  // off-screen buffer.
  std::function<const Image8&()> shown_frame = {};
  // Moves the pointer to (x, y) (`MOUSE_force_move`: a replayed mouse
  // move); none: the input's own position is kept.
  std::function<void(int, int)> warp_pointer = {};
};

// Builds the window of a `windows.txt` record by its TYPE
// (`WinMgr_load_windows` 0x52a80). `shapes` is the shape table of the
// record's first screen (null for none).
using WindowCreator =
    std::function<std::unique_ptr<GWindow>(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes)>;

// The window classes by `windows.txt` TYPE. The standard set holds the
// generic classes of the toolkit; the TYPEs of screens not written yet make
// placeholders (plain windows) until their class is added.
class WindowFactory {
 public:
  // The toolkit's classes (`docs/07-ui-framework.md`, "Classes and
  // `windows.txt` TYPE values").
  static WindowFactory standard();

  // Makes `create` build the windows of TYPE `type` (replacing any other).
  void add(int type, WindowCreator create);
  // Returns the window of `record`; a TYPE without a class gives a plain
  // window.
  std::unique_ptr<GWindow> create(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) const;

 private:
  std::map<int, WindowCreator> creators_;
};

class WinMgr final : public WindowStateIo {
 public:
  static constexpr int kScreens = 32;
  static constexpr int kScreenWindows = 32;
  static constexpr int kHistory = 16;
  static constexpr int kMaxMessages = 32;
  static constexpr int kMaxTimers = 16;
  static constexpr int kMaxNotifies = 16;
  // The main loop's minimum pass length, in ticks.
  static constexpr std::uint32_t kPassTicks = 3;
  // `WinMgr_set_timer` message 666: the timer calls a function, not a window.
  static constexpr int kTimerCallback = 666;

  // Input notification masks (`WinMgr_request_notify` 0x51b38): a left or
  // right button change (press or release) sends the message with the
  // registered parameters, a mouse move with the pointer's position.
  enum NotifyMask : int { kNotifyLeft = 1, kNotifyRight = 2, kNotifyMove = 4 };

  // `WinMgr_init` (0x529cc) and `WinMgr_load_windows` (0x52a80): reads
  // `windows.txt`, `gwshare.txt`, the haze tables and fonts they name, and
  // creates the windows through `factory`; installs the screens' enter
  // handlers (`main` 0x1c424) and reads `ascend.cfg` (`Config_load`
  // 0x54c38). Throws `DataError` when a file is missing or malformed.
  explicit WinMgr(UiServices services, const WindowFactory& factory = WindowFactory::standard());
  ~WinMgr();
  WinMgr(const WinMgr&) = delete;
  WinMgr& operator=(const WinMgr&) = delete;

  // The bytes of window `w`'s part of the saves' window state, by its class
  // (`windows.txt` TYPE: the classes overriding `GWindow_serialize`): the
  // star map 0xbe, a toggle 4, a button group 14 per control, the star
  // system 0x1a08, the planet 1, the research camera 0x98, others none.
  static std::size_t state_size(const GWindow& w);
  // The window state of the windows as built (`WinMgr_load_windows`): what
  // a new game saves for the windows that leave their part to the window
  // manager (the star system, planet and research windows until written).
  std::vector<std::uint8_t> initial_window_state() const;
  // `WindowStateIo`: the windows' parts into / from the core's window state
  // (`game::Session::window_state`), in window-table order. A window whose
  // `save_state` returns false keeps the bytes already there.
  void store_window_state() override;
  void restore_window_state() override;
  // "Begin New Game" (`NewGameWnd_handle_event` 0x4dd5c): each window's
  // `new_game` reset (the star map camera, the race controls' frames, the
  // star-system views and the research camera orientation), then the race
  // slots' shapes.
  void new_game_windows();
  // `Cosmos_setup_race_palettes` (0x1e818): the shared shapes of race slot
  // `i` (`gwshare.txt` 0..27: small and large portraits, ship sprites) become
  // the files of race `i`'s species, for the races of the game.
  void setup_race_shapes();
  // After a game is loaded from the load list: Esc goes back to the opening
  // menu (the history set to screen 0, `LoadWnd_handle_event` 0x30988).
  void reset_history_to_opening();

  UiServices& services() { return services_; }
  ShapeCache& shapes() { return shapes_; }
  GameFont& small_font() { return *small_font_; }
  GameFont& large_font() { return *large_font_; }
  Image8& screen() { return services_.gsystem.screen(); }
  // The WMGR_MAPFOCUS remap (`+0x3012`): identity with 0xf2 -> 0x96, patched
  // by the toggle buttons while they draw.
  HazeTable& focus_map() { return focus_map_; }
  // The WMGR_MAPRACES remap (`+0x3022`): identity, its entries 0x10..0x13
  // set to a race's colour ramp (`draw_shape_race_colored` 0x4f2c8).
  HazeTable& race_map() { return race_map_; }
  // `gwshare.txt` table `index` (0..44, `GWindow_load_shared_shapes`
  // 0x2c9ec), loaded at the first use.
  const ShapeTable& shared_shapes(int index);

  // `static_string` (0x1cea8): string `index` of `static.txt` (loaded at
  // the first use), in the files' language. Throws `std::out_of_range` past
  // the table.
  const std::string& static_string(int index);
  // A text of the remake's own (not of the game files), in the files'
  // language: `src/lang/<language>/openascendance.tsv`.
  std::string remake_text(std::string_view english) const { return services_.files.text(kRemakeTexts, english); }
  static constexpr std::string_view kRemakeTexts = "openascendance.txt";

  // Whether the game runs (`0x865f4`; message 2 and Alt+X clear it).
  bool running() const { return running_; }
  void quit() { running_ = false; }
  int current_screen() const { return current_; }
  // The hot (mouse-over) window id (`+0x3002`), -1 for none.
  int hot_window() const { return hot_; }
  bool help_mode() const { return help_mode_; }
  // The input of the current pass (the pointer position, `0xc84d4`).
  const InputState& input() const { return last_input_; }
  bool in_modal() const { return modal_; }
  // The remake's enlarged click targets (not in the original), how far
  // around its rectangle a click target takes the pointer, in pixels.
  static constexpr int kTargetMargin = 5;
  // The point where the pointer at `raw` acts (hovers and presses): when no
  // shown window of the current screen takes a press there
  // (`GWindow::press_areas`: the original would deliver the press to no
  // window) and exactly one click target's rectangle grown by
  // `kTargetMargin` on each side holds it, the nearest point of that
  // target, provided that the target's window is the top one taking a
  // press there; else `raw` itself. Always `raw` while a trace is replayed
  // or when the enlarged targets are off.
  PointerPos target_point(PointerPos raw) const;
  // Turns the enlarged click targets on (the default) or off (the
  // original's targets, as for the input of an oracle run).
  void set_enlarged_targets(bool on) { enlarged_targets_ = on; }
  // Whether the remake's enlarged targets act now: on and no trace
  // replayed (the windows' own enlarged picking, such as the star map's,
  // follows it).
  bool enlarged_targets() const { return enlarged_targets_ && !replaying_; }
  // The id of the top shown window of the current screen taking a view
  // gesture at (x, y) (`GWindow::takes_view_gesture`), 0 for none or in a
  // modal loop or help mode.
  int view_window_at(int x, int y, bool mouse) const;
  // Whether a shown window of the current screen plays an animation that
  // Space skips (`GWindow::plays_skippable_animation`), not in a modal loop:
  // a finger put down is then Space (a remake addition, `TouchMapper`).
  bool skippable_animation() const;
  // What a finger's long press at (x, y) is (a remake addition,
  // `TouchMapper`): the answer of the top shown window of the current
  // screen taking a press at its target point (`GWindow::long_press`), a
  // help click when none does or in a modal loop (where it is a left click,
  // as the original's Shift + click).
  LongPress long_press(int x, int y) const;
  // The exit corner under the pointer (`+0x24da8`): 0..3, -1 for none.
  int exit_corner() const { return exit_corner_; }
  // The auto next turn (`+0x24d9c`, NEXTTURNCONT, message 8): a next day is
  // asked at each pass without game events.
  bool auto_turn() const { return auto_turn_; }
  // peCustom: whether the star map leaves the selected star's ring out (the
  // blink after a ship's game event, `blink_step`).
  bool selection_ring_hidden() const { return blink_passes_ > 0 && (blink_passes_ - 1) / kBlinkPhasePasses % 2 == 1; }
  void set_auto_turn(bool on) { auto_turn_ = on; }
  // The species the player deals with (`0x8675d`: the diplomacy and
  // intelligence screens' race slot).
  int target_race() const { return target_race_; }
  void set_target_race(int race) { target_race_ = race; }
  // `WinMgr_set_pointer` (0x55680): the pointer frame, kept in help mode.
  void set_pointer(int frame);
  // The rotation speed factor (`+0x24dac`, saved in `ascend.cfg`).
  float rotation_speed() const { return rotation_speed_; }
  void set_rotation_speed(float speed) { rotation_speed_ = speed; }
  // `draw_shape_race_colored` (0x4f2c8) / `draw_shape_colored` (0x4f350):
  // draws `shape` at (x, y) of `pane` with its entries 0x10..0x13 remapped
  // to the ramp of colour `color` (0x10 + 4 * color ..).
  void draw_colored(const Pane& pane, const Shape& shape, int x, int y, int color);
  // Whether the next entry to the opening menu loads `resume.gam`
  // (`0x86a4c`: at start, and after the new-game screen opened until Begin).
  void set_reload_resume(bool reload) { first_opening_ = reload; }
  // The screen history (`+0x4736`): the screens Esc goes back to, oldest
  // first.
  std::vector<int> history() const;

  // Returns window `id` (1-based), null when there is none.
  GWindow* window(int id) const;
  // `WinMgr_find_window` (0x521b8): the window named `name` (first 10
  // characters, case-insensitive), null when there is none.
  GWindow* find_window(std::string_view name) const;
  // `WinMgr_register_window` (0x525c8): takes a window made by the code
  // (dialogs) and returns its id.
  int register_window(std::unique_ptr<GWindow> window);
  // `WinMgr_release_window` (0x52940): destroys window `id`.
  void release_window(int id);
  // `WinMgr_add_to_screen` (0x52630) / `WinMgr_remove_from_screen`
  // (0x52720): puts window `id` in the first free slot of `screen`'s list,
  // or takes it out; return false when the list is full or `id` is not in
  // it.
  bool add_to_screen(int id, int screen);
  bool remove_from_screen(int id, int screen);

  // Clears the hot window when it is `id` (a deactivated window).
  void release_hot(int id);

  // `WinMgr_set_enter_handler` / `WinMgr_set_leave_handler` (0x52900,
  // 0x52920): called with the screen when it is entered (after the wipe,
  // before the windows' activation) or left.
  void set_enter_handler(int screen, std::function<void(int)> handler);
  void set_leave_handler(int screen, std::function<void(int)> handler);
  // `screen_draw_background` (0x55730): the `.tmp` background of `screen`.
  void draw_background(int screen);

  // `WinMgr_set_screen` (0x522ac): switches to `screen`, pushing the current
  // one on the history with `push`.
  void set_screen(int screen, bool push);
  // `WinMgr_go_back` (0x524bc): pops the history and switches to it.
  void go_back();
  // `WinMgr_post_message` (0x51f70): queues a system message (dropped when
  // the queue is full). `text` is the help topic of message 5.
  void post_message(int id, int p1, int p2, std::string text = {});
  // `WinMgr_send_event` (0x52140): `handle_event` of window `id`.
  bool send_event(int id, int type, int a, int b);
  // `WinMgr_broadcast_event` (0x51ff8): to the windows of the current screen
  // from the top down until one consumes it.
  bool broadcast_event(int type, int a, int b);
  // `WinMgr_set_timer` (0x51810): sends event `msg` (p1, p2) to window `id`
  // every `period` ticks (each pass for 0; always each pass while a trace
  // is replayed). `WinMgr_kill_timer` (0x518d0)
  // removes the window's timers of `msg` (-1: all).
  void set_timer(int id, int msg, int p1, int p2, std::uint32_t period);
  void kill_timer(int id, int msg);
  // `WinMgr_get_timer_period` / `WinMgr_set_timer_period` (0x51938,
  // 0x51974): the period of window `id`'s timer of `msg`, -1 for none.
  int timer_period(int id, int msg) const;
  void set_timer_period(int id, int msg, std::uint32_t period);
  // `WinMgr_request_notify` (0x51b38) / `WinMgr_cancel_notify` (0x51bcc):
  // sends `msg` to window `id` on the input changes of `mask`, wherever the
  // pointer is.
  void request_notify(int id, int mask, int msg, int p1, int p2);
  void cancel_notify(int id, int msg);

  // `WinMgr_do_modal` (0x50708): runs `dialog` (a registered window) and its
  // children over the current screen until message 6 (or Esc, or a click on
  // an exit corner) ends the loop; returns the result. The screen under the
  // dialog is kept in the game core's shared work buffer
  // (`game::Session::scratch`, which the day can read), but for the
  // remake's own windows (`remake_window`), which keep it apart.
  int do_modal(GWindow& dialog, bool remake_window = false);
  // Ends the running modal loop with `result` (message 6).
  void end_modal(int result);
  // `WinMgr_message_box` (0x50adc): `text` in a box with an OK button, run
  // modally.
  void message_box(std::string text);
  // `WinMgr_input_box` (0x50be4): `title` and an edit field on `buffer` (at
  // most `max_length` characters), run modally; returns the result (3 for
  // Enter, 201 for OK, 0 for Esc).
  int input_box(std::string_view title, std::string& buffer, int max_length);
  // A remake addition: `title` over a column of buttons, one per choice
  // (`current` marked), run modally; returns the index of the choice
  // clicked, -1 for none (Esc, Back).
  int choice_box(std::string_view title, const std::vector<std::string>& choices, int current);

  // Arms the pending palette fade (`0x122034`): `fade_to_palette_step` 0..64,
  // one per refresh.
  void start_pending_fade();
  void cancel_pending_fade() { fade_pending_ = false; }

  // `WinMgr_process_input` (0x4faac): one main-loop pass of input handling,
  // messages, the music picker and the wait until 3 ticks have passed since
  // the previous pass.
  void process_input(const InputState& input);
  // The system's Back (a remake addition, Android's Back button and
  // gesture): what a click on an exit corner does (the modal loop ended
  // with -1, else the exit sound and the previous screen), and Exit to DOS
  // on the opening menu, which has no exit corners. An action in progress
  // on the screen is cancelled first, the screen kept (`cancel_action`).
  void system_back();

  // `WinMgr_refresh_screen` (0x5049c): advances the pending fade.
  void refresh_screen();
  // One pass of the main loop (`main` 0x1c424, `WinMgr_run_modal_loop`
  // 0x50a28): input, `process_input`, `refresh_screen`, presentation.
  void run_pass();

  // `Config_save` (0x54cac): writes `ascend.cfg` (the mute flags and the
  // rotation speed).
  void save_config() const;

  // `Tutorial_start_replay` (0x54d98): from the next pass, the input comes
  // from the window-event trace `trace` (the file `name`) instead of the
  // player, one pass per counted pass; a recorded Ctrl+P opens the next
  // `info%02d` topic of the help file named as the trace with the extension
  // `txt`, and a real key or click asks whether to end the replay. With
  // `loop` (the developer's message 4) the trace starts over at its end,
  // else the replay stops there. Returns false (nothing done) while a
  // replay runs. Throws `DataError` when `trace` is not a trace.
  bool start_replay(std::span<const std::uint8_t> trace, std::string_view name, bool loop);
  // A tutorial picked in the tutorial list (`LoadWnd_handle_event`
  // 0x30988): `<name>.tsv` loaded, back to the previous screen, `srand(0)`,
  // the rotation speed 1 (the player's restored when the tutorial is
  // ended), then `<name>.bin` replayed. Returns false when a file is
  // missing or not readable.
  bool start_tutorial(const std::string& name);
  // Whether a trace is being replayed (`+0x48aa`).
  bool replaying() const { return replaying_; }
  // The counted main-loop passes of the replay (`+0x48b6`).
  std::uint32_t replay_pass() const { return replay_pass_; }

 private:
  // The buffer `do_modal` and the input box save the screen under their
  // window into (`0xc8c2c`): the game core's `game::Session::scratch`, or
  // `own_scratch_` for the remake's own windows or without a game core.
  std::span<std::uint8_t> scratch_buffer(bool remake_window);

  struct Screen {
    std::array<int, kScreenWindows> slots{};  // window ids, 0 = free; slot 31 on top
    int count = 0;
    std::string shape_file;  // TYPE 100 SHAPEFILE
    std::function<void(int)> enter;
    std::function<void(int)> leave;
  };
  struct Timer {
    int window = 0;  // 0 = free
    int msg = 0;
    int p1 = 0;
    int p2 = 0;
    std::uint32_t period = 0;
    std::uint32_t due = 0;
  };
  struct Notify {
    int window = 0;  // 0 = free
    int mask = 0;
    int msg = 0;
    int p1 = 0;
    int p2 = 0;
  };
  struct Message {
    int id;
    int p1;
    int p2;
    std::string text;
  };

  void load_windows(const WindowsTxt& txt, const WindowFactory& factory);
  void install_screen_handlers();
  void load_config();
  Screen& screen_record(int screen);
  // `WinMgr_broadcast_event` with its modal filter (`0x12200c`, one flag
  // per slot): with `filter`, only the slots flagged get the event, and
  // the flags are set to whether the window took it.
  bool broadcast(int type, int a, int b, bool filter);
  void update_hover(const InputState& input);
  void run_timers();
  void run_notifies(const InputState& input);
  // `WinMgr_handle_hotkey` (0x50174): returns false when the key is not a
  // hotkey (it is then broadcast).
  bool handle_hotkey(const InputState& input);
  void dispatch_messages();
  void show_help(const Message& m);
  void pick_music(const InputState& input);
  // Message 7: the next contested star's event, else the end of the day.
  void next_turn();
  // Message 8: the auto next turn toggled, or the next contested star's
  // event.
  void auto_turn_message();
  // The remake's view gestures: the window under a gesture's start that
  // takes it gets its drags and pinches (none in a modal loop or help
  // mode).
  void route_view_gesture(const ViewGesture& gesture);
  // The remake's mouse wheel: its notches to the window that would take a
  // finger's gesture at (x, y), on the same conditions.
  void route_wheel(int x, int y, int notches);

  // `WinMgr_show_game_events` (0x50f84): the next queued game event in
  // `EVENTWIN`, run modally, then the screen its answer leads to.
  void show_game_events();
  // The screen the answer `result` (a button's MSG, 0 for Esc) to `event`
  // leads to, 0 for none, with the selections it makes (the star, planet
  // or species of the screen).
  int game_event_screen(const game::GameEvent& event, int result);
  // peCustom: the star a game event of a ship (in a system or entering a
  // planet's orbit) is at, -1 for none or another variant.
  int event_star(const game::GameEvent& event) const;
  // peCustom: a pass of the selected star's blink (the map redrawn when the
  // ring shows or hides), at its end the screen the event's answer chose.
  void blink_step();
  // `WinEvent_replay` (0x54f90): the input of a replayed pass.
  const InputState& replay_input();
  // `input_inject_event` (0x25cc8): one record into the replayed input.
  void inject(const TraceRecord& record);
  // `WinMgr_check_replay_abort` (0x54e64): after a replayed pass, the real
  // input is read; a key or a button press asks `endtut`.
  void check_replay_abort();
  // The Ctrl+P of a replay: the next tutorial help box, run modally with
  // the replay paused.
  void show_tutorial_box();
  // Alt+G (`screenshot_save_gif` 0x31338): `scr%03d.gif` in the user folder,
  // of the picture on the screen (the frame of the previous pass, with the
  // pointer).
  void save_screenshot();

  UiServices services_;
  ShapeCache shapes_;
  std::vector<std::string> shared_shape_files_;  // gwshare.txt
  std::vector<std::string> static_strings_;      // static.txt
  std::unique_ptr<GameFont> small_font_;
  std::unique_ptr<GameFont> large_font_;
  HazeTable focus_map_{};
  HazeTable race_map_{};
  std::vector<std::unique_ptr<GWindow>> windows_;  // id - 1; null once released
  std::array<Screen, kScreens> screens_;
  int current_ = -1;
  // The history ring (`+0x4736`, count `+0x4756`, top `+0x4758`).
  std::array<int, kHistory> history_{};
  int history_count_ = 0;
  int history_top_ = 0;
  std::vector<Message> messages_;
  std::array<Timer, kMaxTimers> timers_{};
  std::array<Notify, kMaxNotifies> notifies_{};
  std::array<bool, kScreenWindows> modal_flags_{};  // 0x12200c
  int hot_ = -1;
  bool running_ = true;
  bool modal_ = false;      // +0x489e
  int modal_result_ = 0;    // +0x48a2
  bool help_mode_ = false;  // +0x24da4
  int exit_corner_ = -1;    // +0x24da8
  bool auto_turn_ = false;  // +0x24d9c (NEXTTURNCONT)
  int target_race_ = 0;     // 0x8675d
  // Whether `do_modal` saves and restores the screen under the dialog
  // (`0x86a44`; the input box does its own).
  bool save_under_ = true;
  // The buffer of the screen under a modal window when there is no game
  // core, or for the remake's own windows.
  std::vector<std::uint8_t> own_scratch_;
  float rotation_speed_ = 1.0F;
  bool fade_pending_ = false;
  int fade_step_ = 0;
  bool first_opening_ = true;  // `0x86a4c`: resume.gam not loaded yet
  int screenshot_count_ = 0;
  // The music picker (`0x86a3c`, `0x86a40`).
  int previous_tune_ = -1;
  int volume_ramp_ = -1;
  std::uint32_t last_pass_tick_ = 0;  // `0x122008`
  // The input globals of the pass (`0xc8484..`): nested passes (modal
  // loops) overwrite them, and a modal loop clears the key and button
  // changes when it ends.
  InputState last_input_;
  // The replay (`+0x48aa` replaying, `+0x48ae` stop at the end, `+0x48b6`
  // pass counter, `+0x48ba` records, `+0x48be` next record).
  bool replaying_ = false;
  bool replay_loops_ = false;
  std::uint32_t replay_pass_ = 0;
  Trace trace_;
  std::size_t replay_next_ = 0;
  InputState replay_state_;
  // The tutorial's help file (`0x121ff4`) and the number of its boxes
  // shown (`0x121ff0`).
  std::string tutorial_help_;
  int tutorial_boxes_ = 0;
  // The rotation speed before the tutorial (`0x86a48`).
  float saved_rotation_speed_ = 1.0F;
  // `0xfa340`: a game event box is being prepared.
  bool showing_event_ = false;
  // peCustom: after a ship's game event the selected star's ring blinks
  // (`kBlinkPhases` phases, shown, hidden, shown, ..., of `kBlinkPhasePasses`
  // passes each) before the screen chosen by the answer (0 none) comes.
  static constexpr int kBlinkPhasePasses = 8;
  static constexpr int kBlinkPhases = 5;
  int blink_passes_ = 0;
  int blink_screen_ = 0;
  // The window a view gesture goes to (a remake addition), 0 for none.
  int gesture_window_ = 0;
  // The pointer's position given by the input, before `target_point` moves
  // `last_input_`'s (a remake addition).
  PointerPos raw_pointer_;
  // Whether the pointer acted at another point than its own at the last
  // pass.
  bool pointer_on_target_ = false;
  // The top shown window of the current screen taking a press at `p`
  // (the broadcast's order), null for none.
  const GWindow* press_receiver(PointerPos p) const;
  // Esc or Back outside a modal loop (a remake addition, off while a trace
  // is replayed): cancels the current screen's action in progress
  // (`GWindow::cancel_action`); returns whether one was.
  bool cancel_action();

  bool enlarged_targets_ = true;
};

}  // namespace oa
