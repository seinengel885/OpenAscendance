#include "ui/winmgr.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>

#include "assets/gif.h"
#include "assets/static_strings.h"
#include "assets/tokens.h"
#include "core/c_format.h"
#include "core/error.h"
#include "core/log.h"
#include "game/address.h"
#include "game/raceneg.h"
#include "game/session.h"
#include "game/tables.h"
#include "ui/button_group.h"
#include "ui/edit_field.h"
#include "ui/event_wnd.h"
#include "ui/help_wnd.h"
#include "ui/text_box.h"
#include "ui/toggle_button.h"

namespace oa {

namespace {

// BIOS scan codes of the hotkeys (`WinMgr_handle_hotkey` 0x50174).
constexpr std::uint16_t kScanEsc = 0x01;
constexpr std::uint16_t kScanS = 0x1f;
constexpr std::uint16_t kScanG = 0x22;
constexpr std::uint16_t kScanL = 0x26;
constexpr std::uint16_t kScanX = 0x2d;
constexpr std::uint16_t kScanN = 0x31;
constexpr std::uint16_t kScanM = 0x32;
// Ctrl+P of a replayed trace: the next tutorial help box.
constexpr std::uint16_t kScanP = 0x19;

// The screens of the hotkeys and the screen-specific rules.
constexpr int kScreenOpening = 0;
constexpr int kScreenStarMap = 1;
constexpr int kScreenSave = 2;
constexpr int kScreenLoad = 4;
constexpr int kScreenShipDesign = 8;
constexpr int kScreenEndOfGame = 9;
constexpr int kScreenNegotiation = 13;
constexpr int kScreenStarSystem = 16;
constexpr int kScreenPlanet = 17;
constexpr int kScreenResearch = 20;

// The developer's trace (message 4) and the tutorials' end question.
constexpr std::string_view kDeveloperTrace = "WINEVENT.BIN";
constexpr std::string_view kEndTutorialTopic = "endtut";

// The themes the music picker chooses from (tunes 0..4).
constexpr int kThemes = 5;
// The resumed theme's fade-in ends at this volume.
constexpr int kResumeVolume = 100;

// The window classes with a part in the saves' window state (`windows.txt`
// TYPE) and their sizes.
constexpr int kTypeStarMap = 2;
constexpr int kTypeButtonGroup = 3;
constexpr int kTypeStarSystem = 4;
constexpr int kTypePlanet = 7;
constexpr int kTypeResearch = 9;
constexpr int kTypeToggle = 25;
constexpr std::size_t kStarMapStateSize = 0xbe;
constexpr std::size_t kToggleStateSize = 4;
constexpr std::size_t kStarSystemStateSize = 0x1a08;
constexpr std::size_t kPlanetStateSize = 1;
constexpr std::size_t kCameraStateSize = 0x98;

// The star system window's state (`BatModeWnd_serialize` 0x19e2c): the
// 100 per-star "camera set" flags at +0x12c0 (cleared by Begin New Game).
constexpr std::size_t kStarSystemFlagsOffset = 0x12c0;
constexpr std::size_t kStarSystemFlagsSize = 100 * 4;
// A camera's orientation basis (`Camera` +0x74), reset by Begin New Game in
// the research window's camera.
constexpr std::size_t kCameraOrientationOffset = 0x74;

// Appends the little-endian bytes of `v`.
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(v >> shift));
  }
}
void put_f32(std::vector<std::uint8_t>& out, float v) { put_u32(out, std::bit_cast<std::uint32_t>(v)); }

// The bytes of a camera (`Camera_ctor` 0x1b4f0: identity matrices, the rest
// zero), with `fov`, `focal`, `near`, `far`, `position` and `scale` (all
// zero for a camera only constructed).
std::vector<std::uint8_t> camera_bytes(float fov, float focal, float near_clip, float far_clip,
                                       std::array<float, 3> position, float scale) {
  constexpr std::array<float, 9> kIdentity3 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  constexpr std::array<float, 12> kIdentity34 = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  std::vector<std::uint8_t> out;
  for (const float v : kIdentity3) {
    put_f32(out, v);
  }
  for (const float v : {fov, focal, near_clip, far_clip}) {
    put_f32(out, v);
  }
  for (const float v : position) {
    put_f32(out, v);
  }
  for (const float v : kIdentity34) {
    put_f32(out, v);
  }
  put_f32(out, scale);
  for (const float v : kIdentity3) {
    put_f32(out, v);
  }
  return out;
}

// The state of the windows that leave it to the window manager, as built:
// the star system view's (`BatModeWnd` ctor and init: 100 object slots with
// identity rotations, a constructed camera, the view basis, the screen
// centre (229, 232) and the five toggles -1, 0, -1, -1, -1), the planet
// window's display flags (5) and the research camera (`ResWnd_init`
// 0x4250c: fov 30, focal 320 / tan 15 degrees, near 1, far 100000, at
// (0, 0, -150), the view not built yet).
std::vector<std::uint8_t> default_state(int type) {
  std::vector<std::uint8_t> out;
  switch (type) {
    case kTypeStarSystem: {
      out.assign(0x4b0, 0);
      for (int slot = 0; slot < 100; ++slot) {
        for (int k = 0; k < 9; ++k) {
          put_f32(out, k % 4 == 0 ? 1.0F : 0.0F);
        }
      }
      out.resize(0x190c, 0);
      const std::vector<std::uint8_t> camera = camera_bytes(0, 0, 0, 0, {0, 0, 0}, 0);
      out.insert(out.end(), camera.begin(), camera.end());
      for (const float v :
           {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}) {
        put_f32(out, v);
      }
      put_u32(out, 229U | (232U << 16));
      put_u32(out, 0);
      put_f32(out, 0);
      for (const std::uint32_t v : {0xffffffffU, 0U, 0xffffffffU, 0xffffffffU, 0xffffffffU}) {
        put_u32(out, v);
      }
      break;
    }
    case kTypePlanet:
      out.push_back(5);
      break;
    case kTypeResearch: {
      const double focal = 320.0 / std::tan(30.0 * static_cast<double>(0.017453292F) * 0.5);
      out = camera_bytes(30, static_cast<float>(focal), 1, 100000, {0, 0, -150}, 1);
      break;
    }
    default:
      break;
  }
  return out;
}

// The click sound of the exit corners (`soundfx.txt` 0, `button`).
constexpr int kExitSound = 0;

// `ascend.cfg`: the SFX and music mute flags (i32, 0 / -1) and the rotation
// speed (float).
constexpr std::string_view kConfigFile = "ascend.cfg";
constexpr std::size_t kConfigSize = 12;

// The number of `gwshare.txt` names (`GWindow_load_shared_shapes`).
constexpr int kSharedShapes = 45;

// The static strings of the dialogs.
constexpr int kOkString = 0x1e;  // "OK"

// The message box (`WinMgr_message_box` 0x50adc): the box and its button.
constexpr Pane kMessageBoxPane{50, 50, 300, 300};
constexpr Pane kMessageButtonPane{260, 260, 290, 290};
// The input box (`WinMgr_input_box` 0x50be4): its x range, and the frame of
// `help.shp` drawn under it.
constexpr int kInputBoxX0 = 0xf2;
constexpr int kInputBoxX1 = 0x18d;
constexpr int kInputBoxFrame = 1;
constexpr int kHelpShapes = 37;
// `gwshare.txt`: the race slots' small ships, the gizmos and `planitem.shp`.
constexpr int kShipShapes = 14;
constexpr int kGizmoShapes = 28;
constexpr int kPlanItemShapes = 30;

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// The size of the shared work buffer (`save_screen_under` asserts the
// rectangle fits).
constexpr std::size_t kScratchBytes = game::Session::kScratchSize;

// Copies the screen under `pane` into the start of `buffer`
// (`save_screen_under` 0x4f618): `vfx_pane_copy` to a VFX window over the
// buffer whose maximum coordinates (the window's +4 and +8, not its size)
// are `x1 - x0` and `y1 - y0`, so the whole pane, its rows one after the
// other; the assertion only bounds `(x1 - x0) * (y1 - y0)`.
void save_under(const Image8& screen, const Pane& pane, std::span<std::uint8_t> buffer) {
  const int w = pane.x1 - pane.x0 + 1;
  const int h = pane.y1 - pane.y0 + 1;
  if (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > buffer.size()) {
    throw std::logic_error(std::format("{}x{} pixels under a modal window", w, h));
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (screen.contains(pane.x0 + x, pane.y0 + y)) {
        buffer[static_cast<std::size_t>(y * w + x)] = screen.at(pane.x0 + x, pane.y0 + y);
      }
    }
  }
}

// Copies the start of `buffer` back under `pane` (`restore_screen_under`
// 0x4f6c0), the same layout.
void restore_under(Image8& screen, const Pane& pane, std::span<const std::uint8_t> buffer) {
  const int w = pane.x1 - pane.x0 + 1;
  const int h = pane.y1 - pane.y0 + 1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (screen.contains(pane.x0 + x, pane.y0 + y)) {
        screen.set(pane.x0 + x, pane.y0 + y, buffer[static_cast<std::size_t>(y * w + x)]);
      }
    }
  }
}

}  // namespace

WinMgr::WinMgr(UiServices services, const WindowFactory& factory)
    : services_(std::move(services)), shapes_(services_.files) {
  WindowsTxt txt = parse_windows_txt(services_.files.load("windows.txt"));
  for (WindowRecord& record : txt.windows) {
    for (WindowItem& item : record.items) {
      if (item.kind == 0) {
        item.text = services_.files.text("windows.txt", item.text);
      }
    }
  }
  std::vector<HazeTable>& haze = services_.gsystem.haze_tables();
  haze.clear();
  for (const std::string& file : txt.haze_tables) {
    haze.push_back(parse_haze_table(services_.files.load(file), file));
  }
  // The fonts of windows.txt get background 0.
  small_font_ = std::make_unique<GameFont>(parse_font(services_.files.load(txt.small_font), txt.small_font), 0);
  large_font_ = std::make_unique<GameFont>(parse_font(services_.files.load(txt.large_font), txt.large_font), 0);
  for (std::size_t i = 0; i < focus_map_.size(); ++i) {
    focus_map_[i] = static_cast<std::uint8_t>(i);
    race_map_[i] = static_cast<std::uint8_t>(i);
  }
  focus_map_[GWindow::kBackColor] = GWindow::kHotBackColor;
  {
    const std::vector<std::uint8_t> data = services_.files.load("gwshare.txt");
    TokenReader r(data, "gwshare.txt");
    for (int i = 0; i < kSharedShapes; ++i) {
      shared_shape_files_.push_back(r.word());
    }
  }
  for (const auto& [screen, file] : txt.screen_shapes) {
    if (screen < 0 || screen >= kScreens) {
      throw DataError(std::format("windows.txt: screen {} out of range", screen));
    }
    screens_[static_cast<std::size_t>(screen)].shape_file = file;
  }
  load_windows(txt, factory);
  install_screen_handlers();
  load_config();
  services_.game.attach_windows(this);
}

WinMgr::~WinMgr() { services_.game.attach_windows(nullptr); }

std::size_t WinMgr::state_size(const GWindow& w) {
  switch (w.type()) {
    case kTypeStarMap:
      return kStarMapStateSize;
    case kTypeButtonGroup:
      if (const auto* group = dynamic_cast<const ButtonGroup*>(&w)) {
        return group->controls().size() * ButtonGroup::kControlRecordSize;
      }
      return 0;
    case kTypeStarSystem:
      return kStarSystemStateSize;
    case kTypePlanet:
      return kPlanetStateSize;
    case kTypeResearch:
      return kCameraStateSize;
    case kTypeToggle:
      return kToggleStateSize;
    default:
      return 0;
  }
}

std::vector<std::uint8_t> WinMgr::initial_window_state() const {
  std::vector<std::uint8_t> state;
  for (const auto& w : windows_) {
    const std::size_t size = w ? state_size(*w) : 0;
    if (size == 0) {
      continue;
    }
    std::vector<std::uint8_t> part(size, 0);
    if (!w->save_state(part)) {
      part = default_state(w->type());
      part.resize(size, 0);
    }
    state.insert(state.end(), part.begin(), part.end());
  }
  return state;
}

void WinMgr::store_window_state() {
  game::Session* core = services_.game.core();
  if (core == nullptr) {
    return;
  }
  std::vector<std::uint8_t> initial = initial_window_state();
  if (core->window_state.size() != initial.size()) {
    core->window_state = std::move(initial);
    return;
  }
  std::size_t at = 0;
  for (const auto& w : windows_) {
    const std::size_t size = w ? state_size(*w) : 0;
    if (size != 0) {
      w->save_state(std::span(core->window_state).subspan(at, size));
      at += size;
    }
  }
}

void WinMgr::restore_window_state() {
  setup_race_shapes();
  broadcast_event(kEventRefresh, 0, 0);
  game::Session* core = services_.game.core();
  if (core == nullptr) {
    return;
  }
  std::size_t total = 0;
  for (const auto& w : windows_) {
    total += w ? state_size(*w) : 0;
  }
  if (core->window_state.size() != total) {
    log_warning("window state of {} bytes, the windows have {}", core->window_state.size(), total);
    return;
  }
  std::size_t at = 0;
  for (const auto& w : windows_) {
    const std::size_t size = w ? state_size(*w) : 0;
    if (size != 0) {
      w->load_state(std::span<const std::uint8_t>(core->window_state).subspan(at, size));
      at += size;
    }
  }
}

void WinMgr::new_game_windows() {
  game::Session* core = services_.game.core();
  if (core != nullptr) {
    store_window_state();
  }
  std::size_t at = 0;
  for (const auto& w : windows_) {
    if (!w) {
      continue;
    }
    w->new_game();
    const std::size_t size = state_size(*w);
    if (size == 0) {
      continue;
    }
    std::vector<std::uint8_t> scratch(size);
    if (core != nullptr && !w->save_state(scratch)) {
      // The parts kept here: the star system's flags, the research camera's
      // orientation.
      const std::span<std::uint8_t> part = std::span(core->window_state).subspan(at, size);
      if (w->type() == kTypeStarSystem) {
        std::fill_n(part.begin() + kStarSystemFlagsOffset, kStarSystemFlagsSize, 0);
      } else if (w->type() == kTypeResearch) {
        const std::vector<std::uint8_t> identity = camera_bytes(0, 0, 0, 0, {0, 0, 0}, 0);
        std::copy(identity.begin() + kCameraOrientationOffset, identity.end(), part.begin() + kCameraOrientationOffset);
      }
    }
    at += size;
  }
  setup_race_shapes();
}

void WinMgr::setup_race_shapes() {
  const GameSession& game = services_.game;
  for (int race = 0; race < game.race_count() && race < 7; ++race) {
    const int species = game.race_species(race);
    const auto slot = static_cast<std::size_t>(race);
    shared_shape_files_.at(slot) = std::format("DATA\\SMRACE{:02d}.SHP", species);
    shared_shape_files_.at(slot + 7) = std::format("DATA\\LGRACE{:02d}.SHP", species);
    shared_shape_files_.at(slot + 14) = std::format("DATA\\SMSHIP{:02d}.SHP", species);
    shared_shape_files_.at(slot + 21) = std::format("DATA\\DKSHIP{:02d}.SHP", species);
  }
}

void WinMgr::reset_history_to_opening() {
  history_[0] = kScreenOpening;
  history_count_ = 1;
  history_top_ = 1;
  // The same write clears the game-event count.
  if (game::Session* core = services_.game.core()) {
    core->events.clear();
  }
}

void WinMgr::load_windows(const WindowsTxt& txt, const WindowFactory& factory) {
  for (const WindowRecord& record : txt.windows) {
    const ShapeTable* table = nullptr;
    if (!record.screens.empty()) {
      const int first = record.screens.front();
      if (first < 0 || first >= kScreens) {
        throw DataError(std::format("windows.txt: {} on screen {}", record.name, first));
      }
      const std::string& file = screens_[static_cast<std::size_t>(first)].shape_file;
      if (!file.empty()) {
        table = &shapes_.get(file);
      }
    }
    GWindow* parent = nullptr;
    if (upper(record.parent) != "NONE") {
      parent = find_window(record.parent);
      if (parent == nullptr) {
        throw DataError(std::format("windows.txt: {} has no parent {}", record.name, record.parent));
      }
    }
    std::unique_ptr<GWindow> w = factory.create(*this, record, table);
    GWindow* raw = w.get();
    const int id = register_window(std::move(w));
    if (parent != nullptr) {
      raw->set_parent(parent->id());
      parent->add_child(raw);
    }
    for (const int s : record.screens) {
      if (s < 0 || s >= kScreens || !add_to_screen(id, s)) {
        throw DataError(std::format("windows.txt: {} cannot go on screen {}", record.name, s));
      }
    }
  }
}

void WinMgr::install_screen_handlers() {
  // main 0x1c424: screen 0 shows 0opening.gif, the others their .tmp
  // background.
  set_enter_handler(kScreenOpening, [this](int /*screen*/) {
    const Image8 gif = decode_gif(services_.files.load("DATA\\0OPENING.GIF"), "DATA\\0OPENING.GIF");
    Image8& out = screen();
    for (int y = 0; y < std::min(gif.height(), out.height()); ++y) {
      for (int x = 0; x < std::min(gif.width(), out.width()); ++x) {
        out.set(x, y, gif.at(x, y));
      }
    }
  });
  for (const int s : {1, 2, 3, 4, 7, 8, 9, 10, 12, 13, 14, 15, 16, 18, 20}) {
    set_enter_handler(s, [this](int screen) { draw_background(screen); });
  }
  // starmap_screen_leave 0x21b88: NEXTTURNCONT off (not redrawn).
  set_leave_handler(kScreenStarMap, [this](int /*screen*/) {
    if (auto* auto_turn = dynamic_cast<ToggleButton*>(find_window("NEXTTURNCONT"))) {
      auto_turn->set_state(false);
    }
  });
}

void WinMgr::draw_background(int screen) {
  std::string_view file;
  switch (screen) {
    case 1:
      file = "DATA\\1starmap.tmp";
      break;
    case 2:
    case 4:
    case 10:
    case 14:
    case 15:
    case 18:
      file = "DATA\\10status.tmp";
      break;
    case 3:
      file = "DATA\\3cfgnew.tmp";
      break;
    case 7:
      file = "DATA\\7racesel.tmp";
      break;
    case 8:
      file = "DATA\\8shipdes.tmp";
      break;
    case 9:
      file = "DATA\\9ENDGAME.tmp";
      break;
    case 12:
      file = "DATA\\12start.tmp";
      break;
    case 13:
      file = "DATA\\13negot.tmp";
      break;
    case 16:
      file = "DATA\\16battle.tmp";
      break;
    case 20:
      file = "DATA\\20res.tmp";
      break;
    default:
      return;
  }
  vfx_shape_draw(this->screen(), GSystem::kScreenPane, shapes_.get(file).at(0), 0, 0);
}

void WinMgr::load_config() {
  std::optional<std::vector<std::uint8_t>> data;
  if (!services_.user_dir.empty()) {
    std::ifstream in(services_.user_dir / kConfigFile, std::ios::binary);
    if (in) {
      data.emplace(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  if (!data) {
    // The install's own, the original's current directory.
    data = services_.files.try_load(kConfigFile);
  }
  if (!data || data->size() < kConfigSize) {
    return;
  }
  const auto i32 = [&](std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      v |= static_cast<std::uint32_t>((*data)[at + k]) << (8 * k);
    }
    return v;
  };
  services_.sound.set_sfx_muted(i32(0) != 0);
  services_.sound.set_music_muted(i32(4) != 0);
  rotation_speed_ = std::bit_cast<float>(i32(8));
}

void WinMgr::save_config() const {
  if (services_.user_dir.empty()) {
    return;
  }
  std::vector<std::uint8_t> data;
  const auto put = [&](std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
      data.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
  };
  put(services_.sound.sfx_muted() ? 0xffffffffU : 0U);
  put(services_.sound.music_muted() ? 0xffffffffU : 0U);
  put(std::bit_cast<std::uint32_t>(rotation_speed_));
  std::ofstream out(services_.user_dir / kConfigFile, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) {
    log_warning("cannot write {}", (services_.user_dir / kConfigFile).string());
  }
}

void WinMgr::draw_colored(const Pane& pane, const Shape& shape, int x, int y, int color) {
  for (int k = 0; k < 4; ++k) {
    race_map_[static_cast<std::size_t>(0x13 - k)] = static_cast<std::uint8_t>(color * 4 + 0x13 - k);
  }
  vfx_shape_translate_draw(screen(), pane, shape, x, y, race_map_);
}

const std::string& WinMgr::static_string(int index) {
  if (static_strings_.empty()) {
    static_strings_ = parse_static_strings(services_.files.load("static.txt"));
    for (std::string& s : static_strings_) {
      s = services_.files.text("static.txt", s);
    }
  }
  return static_strings_.at(static_cast<std::size_t>(index));
}

const ShapeTable& WinMgr::shared_shapes(int index) {
  return shapes_.get(shared_shape_files_.at(static_cast<std::size_t>(index)));
}

WinMgr::Screen& WinMgr::screen_record(int screen) {
  if (screen < 0 || screen >= kScreens) {
    throw std::out_of_range(std::format("screen {}", screen));
  }
  return screens_[static_cast<std::size_t>(screen)];
}

std::vector<int> WinMgr::history() const {
  std::vector<int> out;
  for (int k = history_count_; k > 0; --k) {
    out.push_back(history_[static_cast<std::size_t>((history_top_ - k + kHistory) % kHistory)]);
  }
  return out;
}

GWindow* WinMgr::window(int id) const {
  if (id < 1 || static_cast<std::size_t>(id) > windows_.size()) {
    return nullptr;
  }
  return windows_[static_cast<std::size_t>(id - 1)].get();
}

GWindow* WinMgr::find_window(std::string_view name) const {
  const std::string wanted = upper(name.substr(0, std::min<std::size_t>(name.size(), 10)));
  for (const auto& w : windows_) {
    if (w && upper(std::string_view(w->name()).substr(0, std::min<std::size_t>(w->name().size(), 10))) == wanted) {
      return w.get();
    }
  }
  return nullptr;
}

int WinMgr::register_window(std::unique_ptr<GWindow> window) {
  windows_.push_back(std::move(window));
  const int id = static_cast<int>(windows_.size());
  windows_.back()->set_id(id);
  return id;
}

void WinMgr::release_window(int id) {
  if (id < 1 || static_cast<std::size_t>(id) > windows_.size()) {
    return;
  }
  for (Screen& s : screens_) {
    for (int& slot : s.slots) {
      if (slot == id) {
        slot = 0;
        --s.count;
      }
    }
  }
  release_hot(id);
  kill_timer(id, -1);
  cancel_notify(id, -1);
  windows_[static_cast<std::size_t>(id - 1)].reset();
}

bool WinMgr::add_to_screen(int id, int screen) {
  Screen& s = screen_record(screen);
  for (int& slot : s.slots) {
    if (slot == 0) {
      slot = id;
      ++s.count;
      return true;
    }
  }
  return false;
}

bool WinMgr::remove_from_screen(int id, int screen) {
  Screen& s = screen_record(screen);
  for (int& slot : s.slots) {
    if (slot == id) {
      slot = 0;
      --s.count;
      return true;
    }
  }
  return false;
}

void WinMgr::release_hot(int id) {
  if (hot_ == id) {
    hot_ = -1;
  }
}

void WinMgr::set_enter_handler(int screen, std::function<void(int)> handler) {
  screen_record(screen).enter = std::move(handler);
}

void WinMgr::set_leave_handler(int screen, std::function<void(int)> handler) {
  screen_record(screen).leave = std::move(handler);
}

void WinMgr::set_screen(int screen, bool push) {
  if (screen == current_) {
    return;
  }
  Screen& next = screen_record(screen);
  if (current_ >= 0 && screen_record(current_).leave) {
    screen_record(current_).leave(current_);
  }
  if (hot_ != -1) {
    send_event(hot_, kEventMouseLeave, 0, 0);
  }
  broadcast_event(kEventDeactivate, 0, 0);
  if (push && current_ >= 0) {
    history_[static_cast<std::size_t>(history_top_)] = current_;
    if (++history_top_ == kHistory) {
      history_top_ = 0;
    }
    history_count_ = std::min(history_count_ + 1, kHistory);
  }
  current_ = screen;
  gesture_window_ = 0;
  services_.game.set_current_screen(screen);
  if (screen == kScreenOpening && first_opening_ && services_.game.resume_exists()) {
    services_.game.load_resume();
  }
  first_opening_ = false;
  vfx_pane_wipe(services_.gsystem.screen(), GSystem::kScreenPane, 0);
  if (next.enter) {
    next.enter(screen);
  }
  broadcast_event(kEventActivate, 0, 0);
  hot_ = -1;
  // The pointer's target point among the new screen's windows.
  const PointerPos target = target_point(raw_pointer_);
  last_input_.x = target.x;
  last_input_.y = target.y;
  pointer_on_target_ = target != raw_pointer_;
  update_hover(last_input_);
}

void WinMgr::go_back() {
  if (history_count_ == 0) {
    return;
  }
  history_top_ = history_top_ == 0 ? kHistory - 1 : history_top_ - 1;
  const int screen = history_[static_cast<std::size_t>(history_top_)];
  --history_count_;
  set_screen(screen, false);
}

void WinMgr::post_message(int id, int p1, int p2, std::string text) {
  if (messages_.size() < kMaxMessages) {
    messages_.push_back({id, p1, p2, std::move(text)});
  }
}

bool WinMgr::send_event(int id, int type, int a, int b) {
  GWindow* w = window(id);
  return w != nullptr && w->handle_event(type, a, b);
}

bool WinMgr::broadcast_event(int type, int a, int b) { return broadcast(type, a, b, false); }

bool WinMgr::broadcast(int type, int a, int b, bool filter) {
  if (current_ < 0) {
    return false;
  }
  const std::array<int, kScreenWindows> slots = screen_record(current_).slots;
  for (int k = kScreenWindows - 1; k >= 0; --k) {
    const std::size_t slot = static_cast<std::size_t>(k);
    GWindow* w = window(slots[slot]);
    if (w == nullptr || (filter && !modal_flags_[slot])) {
      continue;
    }
    // Hidden windows only get 1 and 0xe; children get all but 1 and 2 (their
    // parent forwards those).
    const bool takes =
        !((!w->shown() && type != kEventActivate && type != kEventShow) || (w->parent() != 0 && type < kEventKey));
    if (filter) {
      modal_flags_[slot] = takes;
    }
    if (takes && w->handle_event(type, a, b)) {
      return true;
    }
  }
  return false;
}

void WinMgr::set_timer(int id, int msg, int p1, int p2, std::uint32_t period) {
  if (replaying_) {
    // A replay runs the timers at each pass: the same passes as recorded.
    period = 0;
  }
  for (Timer& t : timers_) {
    if (t.window == 0) {
      t = Timer{id, msg, p1, p2, period, services_.ticks() + period};
      return;
    }
  }
  log_warning("window {}: no free timer", id);
}

void WinMgr::kill_timer(int id, int msg) {
  for (Timer& t : timers_) {
    if (t.window == id && (msg == -1 || t.msg == msg)) {
      t.window = 0;
    }
  }
}

int WinMgr::timer_period(int id, int msg) const {
  for (const Timer& t : timers_) {
    if (t.window == id && t.msg == msg) {
      return static_cast<int>(t.period);
    }
  }
  return -1;
}

void WinMgr::set_timer_period(int id, int msg, std::uint32_t period) {
  for (Timer& t : timers_) {
    if (t.window == id && t.msg == msg) {
      t.period = period;
    }
  }
}

void WinMgr::run_timers() {
  for (Timer& t : timers_) {
    if (t.window == 0 || t.due > services_.ticks()) {
      continue;
    }
    // The slot is read again after the event: a timer killed by its own
    // event keeps its due time unused.
    send_event(t.window, t.msg, t.p1, t.p2);
    t.due += t.period;
  }
}

void WinMgr::request_notify(int id, int mask, int msg, int p1, int p2) {
  for (Notify& n : notifies_) {
    if (n.window == 0) {
      n = Notify{id, mask, msg, p1, p2};
      return;
    }
  }
  log_warning("window {}: no free notification", id);
}

void WinMgr::cancel_notify(int id, int msg) {
  for (Notify& n : notifies_) {
    if (n.window == id && (msg == -1 || n.msg == msg)) {
      n.window = 0;
    }
  }
}

void WinMgr::run_notifies(const InputState& input) {
  const int buttons = (input.left_changed ? kNotifyLeft : 0) | (input.right_changed ? kNotifyRight : 0);
  const int move = input.moved ? kNotifyMove : 0;
  for (const Notify& n : notifies_) {
    if (n.window == 0) {
      continue;
    }
    const Notify copy = n;
    if ((copy.mask & buttons) != 0) {
      send_event(copy.window, copy.msg, copy.p1, copy.p2);
    }
    if ((copy.mask & move) != 0) {
      send_event(copy.window, copy.msg, input.x, input.y);
    }
  }
}

void WinMgr::start_pending_fade() {
  fade_pending_ = true;
  fade_step_ = 0;
}

void WinMgr::update_hover(const InputState& input) {
  const int x = input.x;
  const int y = input.y;
  if (hot_ != -1) {
    GWindow* hot = window(hot_);
    if (hot != nullptr && hot->contains(x, y)) {
      // Inside the hot window: track its child under the pointer.
      for (GWindow* child : hot->children()) {
        if (child->shown() && child->contains(x, y)) {
          send_event(hot_, kEventMouseMove, x, y);
          send_event(hot_, kEventMouseLeave, 0, 0);
          send_event(child->id(), kEventMouseEnter, 0, 0);
          hot_ = child->id();
          return;
        }
      }
      return;
    }
    send_event(hot_, kEventMouseMove, x, y);
    send_event(hot_, kEventMouseLeave, 0, 0);
    hot_ = -1;
  }
  if (current_ < 0) {
    return;
  }
  const std::array<int, kScreenWindows>& slots = screen_record(current_).slots;
  for (int k = kScreenWindows - 1; k >= 0; --k) {
    GWindow* w = window(slots[static_cast<std::size_t>(k)]);
    if (w != nullptr && w->contains(x, y) && w->shown()) {
      hot_ = w->id();
      send_event(hot_, kEventMouseEnter, 0, 0);
      return;
    }
  }
}

void WinMgr::set_pointer(int frame) {
  // WinMgr_set_pointer 0x55680: help mode keeps its pointer.
  if (!help_mode_) {
    services_.set_pointer_frame(frame);
  }
}

bool WinMgr::handle_hotkey(const InputState& input) {
  switch (input.scan_code) {
    case kScanEsc:
      if (modal_) {
        end_modal(0);
        return true;
      }
      if (current_ == kScreenPlanet) {
        return false;  // the planet window takes it
      }
      if (!cancel_action()) {
        go_back();
      }
      return true;
    case kScanS:
    case kScanL:
      if (!modal_ && current_ != kScreenShipDesign) {
        set_screen(input.scan_code == kScanS ? kScreenSave : kScreenLoad, true);
      }
      return true;
    case kScanG:
      save_screenshot();
      return true;
    case kScanX:
      if (!modal_) {
        running_ = false;
      }
      return true;
    case kScanN:
      services_.sound.set_sfx_muted(!services_.sound.sfx_muted());
      return true;
    case kScanM:
      services_.sound.set_music_muted(!services_.sound.music_muted());
      return true;
    default:
      // The developer keys (W, E, R, P, D, F) need `nougat.lf`.
      return false;
  }
}

void WinMgr::save_screenshot() {
  if (services_.user_dir.empty()) {
    return;
  }
  const std::filesystem::path path = services_.user_dir / std::format("scr{:03d}.gif", screenshot_count_);
  const std::vector<std::uint8_t> gif =
      encode_gif(services_.shown_frame ? services_.shown_frame() : screen(), services_.gsystem.dac());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(gif.data()), static_cast<std::streamsize>(gif.size()));
  if (!out) {
    log_warning("cannot write {}", path.string());
    return;
  }
  ++screenshot_count_;
  log_info("screenshot saved to {}", path.string());
}

void WinMgr::show_help(const Message& m) {
  auto* help = dynamic_cast<HelpWnd*>(find_window("HELPWINDOW"));
  if (help == nullptr) {
    throw DataError("windows.txt: no HELPWINDOW");
  }
  switch (m.p2) {
    case kHelpTopic:
      // Without a topic the window runs as it was prepared (the planet
      // screen's colony and invasion reports).
      if (!m.text.empty()) {
        help->open_topic("help.txt", m.text);
      }
      break;
    case kHelpNumbered:
      help->open_topic("help.txt", std::format("help{}", m.p1));
      break;
    case kHelpNoIntro:
      help->open_topic("help.txt", "NOINTRO");
      break;
    case kHelpAbility: {
      // The special abilities window (`AbilityWnd_show_help` 0x32fa4): the
      // topic `abil%02d` of the player's ability; the map is redrawn after.
      const game::Session* core = services_.game.core();
      help = dynamic_cast<HelpWnd*>(find_window("ABILITYWIN"));
      if (core == nullptr || help == nullptr) {
        return;
      }
      help->open_topic("help.txt", std::format("abil{:02d}", core->game->cosmos.races[core->player_race].ability));
      break;
    }
    case kHelpPlanet: {
      // A planet item (the planet screen's build list): its name in the
      // text, its picture (a Ship: the player's small ship).
      const game::Session* core = services_.game.core();
      if (core == nullptr) {
        return;
      }
      help->open_topic("planhelp.txt", std::format("plitem{:02d}", m.p1));
      HelpTopic& topic = help->content();
      if (topic.element_count < 2 || topic.elements[0].kind != HelpElement::Kind::kText ||
          topic.elements[1].kind != HelpElement::Kind::kShape) {
        throw DataError(std::format("planhelp.txt: plitem{:02d} is not a text and a picture", m.p1));
      }
      const std::string& name = core->tables->planitems.items.at(static_cast<std::size_t>(m.p1)).name;
      topic.elements[0].text = c_format(topic.elements[0].text, name.c_str());
      if (m.p1 == game::kItemShip) {
        topic.elements[1].shape_index = kShipShapes + services_.game.player_race();
        topic.elements[1].frame = 0;
      } else {
        topic.elements[1].shape_index = kPlanItemShapes;
        topic.elements[1].frame = m.p1;
      }
      break;
    }
    case kHelpGizmo: {
      // A ship component (a help click on the System Display's gizmo list
      // or the ship design's list): its name in the text, its picture.
      const game::Session* core = services_.game.core();
      if (core == nullptr) {
        return;
      }
      help->open_topic("gizhelp.txt", std::format("gizmo{:02d}", m.p1));
      HelpTopic& topic = help->content();
      if (topic.element_count < 2 || topic.elements[0].kind != HelpElement::Kind::kText ||
          topic.elements[1].kind != HelpElement::Kind::kShape) {
        throw DataError(std::format("gizhelp.txt: gizmo{:02d} is not a text and a picture", m.p1));
      }
      const std::string& name = core->tables->gizmos.at(static_cast<std::size_t>(m.p1)).name;
      topic.elements[0].text = c_format(topic.elements[0].text, name.c_str());
      topic.elements[1].shape_index = kGizmoShapes;
      topic.elements[1].frame = m.p1;
      break;
    }
    default:
      log_debug("help kind {} not handled", m.p2);
      return;
  }
  if (!modal_) {
    do_modal(*help);
  }
  if (m.p2 == kHelpAbility) {
    if (GWindow* map = find_window("COSMOSWnd")) {
      send_event(map->id(), kEventRefresh, 0, 0);
    }
  }
}

void WinMgr::dispatch_messages() {
  const std::vector<Message> queue = std::move(messages_);
  messages_.clear();
  for (const Message& m : queue) {
    switch (m.id) {
      case kMessageNone:
        break;
      case kMessageChangeScreen:
        set_screen(m.p1, m.p2 != 0);
        break;
      case kMessageQuit:
        running_ = false;
        break;
      case kMessageBack:
        go_back();
        break;
      case kMessageHelp:
        show_help(m);
        break;
      case kMessageEndModal:
        end_modal(m.p1);
        break;
      case kMessageReplay:
        // The developer's replay of WINEVENT.BIN (looping).
        if (!replaying_) {
          std::optional<std::vector<std::uint8_t>> trace;
          if (!services_.user_dir.empty()) {
            std::ifstream in(services_.user_dir / kDeveloperTrace, std::ios::binary);
            if (in) {
              trace.emplace(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
          }
          if (!trace) {
            trace = services_.files.try_load(kDeveloperTrace);
          }
          if (trace) {
            start_replay(*trace, kDeveloperTrace, true);
          }
        }
        break;
      case kMessageNextTurn:
        next_turn();
        break;
      case kMessageAutoTurn:
        auto_turn_message();
        break;
      default:
        log_warning("system message {} ({}, {}) unknown", m.id, m.p1, m.p2);
        break;
    }
  }
}

void WinMgr::next_turn() {
  GameSession& game = services_.game;
  if (const int star = game.next_contested_star(); star >= 0) {
    // "There are remaining moves": the box's Next Day posts the command
    // again.
    if (game::Session* core = game.core()) {
      core->post_event(game::EventCode::kRemainingMoves, star, auto_turn_ ? kMessageAutoTurn : kMessageNextTurn);
    }
    return;
  }
  if (game.end_day()) {
    // Cosmos_check_game_over 0x1fec4: the end-of-game screen.
    post_message(kMessageChangeScreen, kScreenEndOfGame, 1);
  }
  // (The developer's trade-all of every new day, Alt+E, is left out.)
  setup_race_shapes();
  broadcast_event(kEventRefresh, 0, 0);
  refresh_screen();
}

void WinMgr::auto_turn_message() {
  GameSession& game = services_.game;
  const int star = game.next_contested_star();
  game::Session* core = game.core();
  if (star < 0 || core == nullptr || core->autoplay) {
    auto_turn_ = !auto_turn_;
  } else {
    core->post_event(game::EventCode::kRemainingMoves, star, kMessageAutoTurn);
  }
}

void WinMgr::show_game_events() {
  game::Session* core = services_.game.core();
  if (showing_event_ || core == nullptr || core->events.empty() || current_ != kScreenStarMap) {
    return;
  }
  auto* box = dynamic_cast<EventWnd*>(find_window("EVENTWIN"));
  if (box == nullptr) {
    throw DataError("windows.txt: no EVENTWIN");
  }
  const game::GameEvent event = core->events.front();
  box->set_event(event);
  showing_event_ = true;
  int result = 0;
  if (!core->autoplay) {
    result = do_modal(*box);
  }
  showing_event_ = false;
  // Esc (0) leaves every event without a jump.
  const int screen = result == 0 ? 0 : game_event_screen(event, result);
  core->events.pop();
  if (screen != 0) {
    post_message(kMessageChangeScreen, screen, 1);
  }
}

int WinMgr::game_event_screen(const game::GameEvent& event, int result) {
  using game::EventCode;
  game::Session& core = *services_.game.core();
  game::Cosmos& cosmos = core.game->cosmos;
  switch (event.code) {
    case EventCode::kResearchDone:
    case EventCode::kXenoDigDiscovery:
    case EventCode::kLabWithoutResearch:
      return kScreenResearch;
    case EventCode::kBuildingComplete:
    case EventCode::kFreePopulation:
    case EventCode::kPlanetBuiltUp:
      cosmos.selected_planet = static_cast<game::PlanetIndex>(event.a);
      return kScreenPlanet;
    case EventCode::kEnteredOrbit:
    case EventCode::kShipBuilt:
    case EventCode::kShipRefitted:
      cosmos.selected_planet = static_cast<game::PlanetIndex>(event.b);
      return kScreenPlanet;
    case EventCode::kRaceWantsToTalk:
      target_race_ = static_cast<std::uint8_t>(event.a);
      return kScreenNegotiation;
    case EventCode::kLaneTransmission:
      target_race_ = static_cast<std::uint8_t>(event.b);
      return kScreenNegotiation;
    case EventCode::kRemainingMoves:
      if (result == 1) {
        cosmos.selected_star = static_cast<game::StarIndex>(event.a);
        return kScreenStarSystem;
      }
      // Next Day (or a right click): the command again; the auto next
      // turn's turns on.
      if (event.b == kMessageNextTurn) {
        post_message(kMessageNextTurn, 0, 0);
      } else {
        auto_turn_ = true;
      }
      return 0;
    case EventCode::kShipArrived:
    case EventCode::kFirstContact:
    case EventCode::kHostileShips:
    case EventCode::kHostileColony:
    case EventCode::kOthersAtWar:
    case EventCode::kUnknownShipEntered:
    case EventCode::kHostileShipEntered:
    case EventCode::kSystemDiscovered: {
      const game::Ship& ship = cosmos.ships.at(static_cast<std::size_t>(event.a));
      if (ship.location_kind != game::ShipLocation::kSystem) {
        throw std::logic_error("game event of a ship outside a system");
      }
      const game::StarIndex star = ship.location;
      if ((event.code == EventCode::kFirstContact || event.code == EventCode::kUnknownShipEntered) && result != 2) {
        // Respond: the met species opens the talk, asking for help when it
        // has ships at war at the star.
        game::Race& race = cosmos.races.at(static_cast<std::size_t>(event.b));
        if (game::race_count_ships_at_star(core, race, game::kWar, cosmos.stars.at(static_cast<std::size_t>(star)),
                                           nullptr) == 0) {
          race.pending_action = game::kDipDiscover;
        } else {
          race.pending_action = game::kDipDiscoverAskHelp;
          race.pending_action_arg = game::star_address(core, star);
        }
        target_race_ = static_cast<std::uint8_t>(event.b);
        return kScreenNegotiation;
      }
      cosmos.selected_star = star;
      return kScreenStarSystem;
    }
    case EventCode::kRaceExtinct:
    case EventCode::kShipScrapped:
    case EventCode::kShipLost:
    case EventCode::kColonyInvaded:
    case EventCode::kBasicsKnown:
    case EventCode::kAbilityCharged:
      return 0;
  }
  return 0;
}

bool WinMgr::start_replay(std::span<const std::uint8_t> trace, std::string_view name, bool loop) {
  if (replaying_) {
    return false;
  }
  Trace parsed = parse_trace(trace, name);
  if (parsed.records.empty()) {
    return false;
  }
  trace_ = std::move(parsed);
  // The help file: the trace's name with `txt` for its last three
  // characters.
  tutorial_help_ = std::string(name);
  if (tutorial_help_.size() >= 3) {
    tutorial_help_.replace(tutorial_help_.size() - 3, 3, "txt");
  }
  // The buttons as last read (`0x121fe8`, `0x121fec`).
  replay_state_ = last_input_;
  replaying_ = true;
  replay_pass_ = 0;
  replay_next_ = 0;
  replay_loops_ = loop;
  tutorial_boxes_ = 0;
  return true;
}

bool WinMgr::start_tutorial(const std::string& name) {
  const std::optional<std::vector<std::uint8_t>> trace = services_.files.try_load(name + ".bin");
  if (!trace || !services_.game.load_game_file(name + ".tsv")) {
    log_warning("tutorial {} cannot be read", name);
    return false;
  }
  post_message(kMessageBack, 0, 0);
  services_.game.srand(0);
  saved_rotation_speed_ = rotation_speed_;
  rotation_speed_ = 1.0F;
  return start_replay(*trace, name + ".bin", false);
}

const InputState& WinMgr::replay_input() {
  replay_state_.left_changed = false;
  replay_state_.right_changed = false;
  replay_state_.moved = false;
  replay_state_.key_event = false;
  replay_state_.wheel = 0;
  while (replaying_) {
    const TraceRecord& r = trace_.records[replay_next_];
    if (replay_pass_ < r.time) {
      break;
    }
    if (r.type == TraceEventType::kMouseMove || r.type == TraceEventType::kMouseButton ||
        r.type == TraceEventType::kKey) {
      inject(r);
    }
    if (++replay_next_ >= trace_.records.size()) {
      if (!replay_loops_) {
        // WinEvent_stop_recording 0x54d44.
        replaying_ = false;
        break;
      }
      replay_next_ = 0;
      replay_pass_ = 0;
    }
  }
  return replay_state_;
}

void WinMgr::inject(const TraceRecord& r) {
  InputState& s = replay_state_;
  s.moved = false;
  s.left_changed = false;
  s.right_changed = false;
  s.key_event = false;
  switch (r.type) {
    case TraceEventType::kMouseMove:
      s.x = static_cast<std::int32_t>(r.a);
      s.y = static_cast<std::int32_t>(r.b);
      if (services_.warp_pointer) {
        services_.warp_pointer(s.x, s.y);
      }
      s.moved = true;
      break;
    case TraceEventType::kMouseButton:
      if (r.a != 0) {
        s.right_down = r.b != 0;
        s.right_changed = true;
      } else {
        s.left_down = r.b != 0;
        s.left_changed = true;
        // A help-mode click carries Shift with its press.
        s.shift = r.msg != 0 && r.b != 0;
      }
      break;
    case TraceEventType::kKey:
      s.shift_flags = r.msg;
      s.ascii = static_cast<std::uint16_t>(r.b);
      s.scan_code = static_cast<std::uint16_t>(r.a);
      s.key_event = true;
      s.shift = (r.msg & 3) != 0;
      s.ctrl = (r.msg & 4) != 0;
      s.alt = (r.msg & 8) != 0;
      break;
    default:
      break;
  }
}

void WinMgr::check_replay_abort() {
  if (!replaying_) {
    return;
  }
  const int x = replay_state_.x;
  const int y = replay_state_.y;
  InputState now = services_.poll_input();
  bool keep = true;
  if (now.key_event || now.back || (now.left_changed && now.left_down) || (now.right_changed && now.right_down)) {
    replaying_ = false;
    int result = 0;
    if (!modal_) {
      auto* help = dynamic_cast<HelpWnd*>(find_window("HELPWINDOW"));
      if (help == nullptr) {
        throw DataError("windows.txt: no HELPWINDOW");
      }
      help->open_topic("help.txt", kEndTutorialTopic);
      result = do_modal(*help);
      now = last_input_;
    }
    if (result == 0) {
      replaying_ = true;
    } else {
      services_.game.srand(static_cast<std::uint32_t>(std::time(nullptr)));
      rotation_speed_ = saved_rotation_speed_;
      keep = false;
    }
  }
  if (keep) {
    // The replayed pointer stays; the rest is the real input's (but the
    // remake's Back and gestures, the player's).
    replay_state_ = now;
    replay_state_.moved = false;
    replay_state_.back = false;
    replay_state_.gesture = {};
    replay_state_.wheel = 0;
    replay_state_.x = x;
    replay_state_.y = y;
  }
}

void WinMgr::show_tutorial_box() {
  replaying_ = false;
  auto* help = dynamic_cast<HelpWnd*>(find_window("HELPWINDOW"));
  if (help == nullptr) {
    throw DataError("windows.txt: no HELPWINDOW");
  }
  help->open_topic(tutorial_help_, std::format("info{:02d}", tutorial_boxes_));
  do_modal(*help);
  // The input after the box: its last pass's, the key and button changes
  // cleared (by the modal loop), no Alt and no scan code.
  last_input_.alt = false;
  last_input_.scan_code = 0;
  replay_state_ = last_input_;
  replaying_ = true;
  ++tutorial_boxes_;
}

void WinMgr::end_modal(int result) {
  modal_ = false;
  modal_result_ = result;
}

std::span<std::uint8_t> WinMgr::scratch_buffer(bool remake_window) {
  game::Session* core = services_.game.core();
  if (remake_window || core == nullptr || core->scratch.size() < kScratchBytes) {
    // The remake's own windows may cover the whole screen; the original's
    // keep the bound of its buffer.
    own_scratch_.resize(static_cast<std::size_t>(kScreenWidth) * kScreenHeight);
    return remake_window ? std::span(own_scratch_) : std::span(own_scratch_).first(kScratchBytes);
  }
  return core->scratch;
}

int WinMgr::do_modal(GWindow& dialog, bool remake_window) {
  if (modal_) {
    throw std::logic_error("nested modal loop");
  }
  // The auto next turn stops (and stays off: only the developer's AI test
  // restores it).
  auto_turn_ = false;
  if (auto* next_turn = dynamic_cast<ToggleButton*>(find_window("NEXTTURNCONT"))) {
    next_turn->set_state(false);
    next_turn->draw(kDrawNormal);
  }
  if (hot_ != -1) {
    send_event(hot_, kEventMouseLeave, 0, 0);
  }
  hot_ = -1;
  modal_flags_.fill(true);
  broadcast(kEventHide, 0, 0, true);
  const Pane under = dialog.pane();
  const bool saved = save_under_;
  if (saved) {
    save_under(screen(), under, scratch_buffer(remake_window));
  }
  add_to_screen(dialog.id(), current_);
  for (GWindow* child : dialog.children()) {
    add_to_screen(child->id(), current_);
  }
  dialog.handle_event(kEventModalOpen, 0, 0);
  dialog.handle_event(kEventActivate, 0, 0);

  // WinMgr_run_modal_loop 0x50a28.
  modal_ = true;
  modal_result_ = 0;
  while (running_ && modal_) {
    run_pass();
  }
  last_input_.left_changed = false;
  last_input_.right_changed = false;
  last_input_.key_event = false;

  dialog.handle_event(kEventDeactivate, 0, 0);
  for (GWindow* child : dialog.children()) {
    remove_from_screen(child->id(), current_);
  }
  remove_from_screen(dialog.id(), current_);
  hot_ = -1;
  if (saved) {
    restore_under(screen(), under, scratch_buffer(remake_window));
  }
  broadcast(kEventShow, 0, 0, true);
  save_under_ = true;
  return modal_result_;
}

void WinMgr::message_box(std::string text) {
  auto box = std::make_unique<TextBox>(*this, "MSGBOX", kMessageBoxPane);
  box->set_text(std::move(text));
  TextBox& box_ref = *box;
  const int box_id = register_window(std::move(box));
  auto button = std::make_unique<GWindow>(*this, static_string(kOkString), kMessageButtonPane);
  button->set_mouse_focus(true);
  button->set_message(kEventOk, 0, 0);
  button->set_parent(box_id);
  GWindow& button_ref = *button;
  const int button_id = register_window(std::move(button));
  box_ref.add_child(&button_ref);
  do_modal(box_ref);
  release_window(button_id);
  release_window(box_id);
}

int WinMgr::choice_box(std::string_view title, const std::vector<std::string>& choices, int current) {
  GameFont& font = large_font();
  const int line = font.font().height + 8;
  int width = font.string_width(title);
  for (const std::string& c : choices) {
    width = std::max(width, font.string_width("> " + c));
  }
  width += 40;
  const int n = static_cast<int>(choices.size());
  const int h = line * (n + 1) + 10;
  const int x0 = (kScreenWidth - width) / 2;
  const int y0 = (kScreenHeight - h) / 2;
  auto box = std::make_unique<GWindow>(*this, std::string(title), Pane{x0, y0, x0 + width, y0 + h});
  box->set_text_pos(-1, 5);
  box->set_border(true);
  GWindow& box_ref = *box;
  const int box_id = register_window(std::move(box));
  std::vector<int> buttons;
  for (int i = 0; i < n; ++i) {
    const std::string& c = choices[static_cast<std::size_t>(i)];
    const int y = y0 + line * (i + 1) + 4;
    auto button =
        std::make_unique<GWindow>(*this, i == current ? "> " + c : c, Pane{x0 + 10, y, x0 + width - 10, y + line - 3});
    button->set_text_pos(-1, 3);
    button->set_mouse_focus(true);
    button->set_border(true);
    button->set_message(kMessageToWinMgr, kMessageEndModal, i + 1);
    button->set_parent(box_id);
    GWindow& button_ref = *button;
    buttons.push_back(register_window(std::move(button)));
    box_ref.add_child(&button_ref);
  }
  const int result = do_modal(box_ref, true);
  for (const int id : buttons) {
    release_window(id);
  }
  release_window(box_id);
  return result >= 1 && result <= n ? result - 1 : -1;
}

int WinMgr::input_box(std::string_view title, std::string& buffer, int max_length) {
  const int font_height = large_font().font().height;
  const int h = font_height * 5 + 5;
  const int y0 = (kScreenHeight - h) / 2;
  auto box =
      std::make_unique<GWindow>(*this, std::string(title.substr(0, 0x13)), Pane{kInputBoxX0, y0, kInputBoxX1, y0 + h});
  box->set_text_pos(-1, 5);
  GWindow& box_ref = *box;
  const int box_id = register_window(std::move(box));

  const Pane& p = box_ref.pane();
  const std::string ok = static_string(kOkString);
  const int ok_width = large_font().string_width(ok);
  auto button =
      std::make_unique<GWindow>(*this, ok, Pane{p.x1 - ok_width - 10, p.y1 - font_height - 5, p.x1 - 3, p.y1 - 2});
  button->set_text_pos(-1, 2);
  button->set_mouse_focus(true);
  button->set_border(true);
  button->set_message(kEventOk, 0, 0);
  button->set_parent(box_id);
  GWindow& button_ref = *button;
  const int button_id = register_window(std::move(button));
  box_ref.add_child(&button_ref);

  const int edit_y0 = p.y0 + font_height + 8;
  auto edit = std::make_unique<EditField>(*this, Pane{p.x0 + 15, edit_y0, p.x1 - 15, edit_y0 + font_height + 8});
  edit->set_parent(box_id);
  edit->set_buffer(buffer, max_length);
  EditField& edit_ref = *edit;
  const int edit_id = register_window(std::move(edit));
  box_ref.add_child(&edit_ref);

  // The frame under the box, saved and restored here (not by do_modal).
  const Pane under{p.x0 - 8, p.y0 - 8, p.x1 + 8, p.y1 + 7};
  save_under_ = false;
  save_under(screen(), under, scratch_buffer(false));
  vfx_shape_draw(screen(), under, shared_shapes(kHelpShapes).at(kInputBoxFrame), 0, 0);
  const int result = do_modal(box_ref);
  restore_under(screen(), under, scratch_buffer(false));
  buffer = edit_ref.text();
  release_window(button_id);
  release_window(edit_id);
  release_window(box_id);
  return result;
}

void WinMgr::pick_music(const InputState& input) {
  SoundSystem& sound = services_.sound;
  if (!sound.music_ready() || sound.music_muted()) {
    return;
  }
  if (!sound.tune_playing() && current_ != 6 && current_ != 7 && current_ != 13) {
    int tune = -1;
    bool resume = false;
    if (previous_tune_ != -1 && sound.saved_position(previous_tune_) != 0) {
      resume = true;
      tune = previous_tune_;
      volume_ramp_ = 0;
    }
    if (tune == -1) {
      tune = previous_tune_ == -1
                 ? 0
                 : static_cast<int>((services_.ticks() + static_cast<std::uint32_t>(input.x)) % kThemes);
      if (tune == previous_tune_) {
        tune = (previous_tune_ + 1) % kThemes;
      }
    }
    sound.play_tune(tune, resume);
    previous_tune_ = tune;
  }
  if (volume_ramp_ != -1) {
    sound.set_music_volume(volume_ramp_);
    if (++volume_ramp_ == kResumeVolume) {
      volume_ramp_ = -1;
    }
  }
}

void WinMgr::process_input(const InputState& input) {
  if (replaying_) {
    ++replay_pass_;
  }
  const PointerPos previous{last_input_.x, last_input_.y};
  last_input_ = input;
  // The pass works on the input globals, which nested passes overwrite.
  InputState& in = last_input_;
  // Shift held: help mode.
  if (in.shift && !help_mode_ && !modal_) {
    help_mode_ = true;
    services_.set_pointer_frame(kPointerHelp);
  } else if (help_mode_ && !in.shift) {
    help_mode_ = false;
    services_.set_pointer_frame(kPointerNormal);
    if (hot_ != -1) {
      send_event(hot_, kEventMouseLeave, 0, 0);
      send_event(hot_, kEventMouseEnter, 0, 0);
    }
  }
  // The exit corners: a corner pixel of any screen but the opening menu.
  int corner = -1;
  if (!help_mode_ && !modal_ && current_ != kScreenOpening) {
    const int right = kScreenWidth - 1;
    const int bottom = kScreenHeight - 1;
    if (in.y == 0) {
      corner = in.x == 0 ? 0 : in.x == right ? 1 : -1;
    } else if (in.y == bottom) {
      corner = in.x == 0 ? 2 : in.x == right ? 3 : -1;
    }
  }
  if (corner != exit_corner_) {
    exit_corner_ = corner;
    set_pointer(corner == -1 ? kPointerNormal : kPointerExitCorner + corner);
  }
  // The remake's enlarged click targets: the pointer acts at its target
  // point; a change of that point (the windows changed) is a move.
  raw_pointer_ = PointerPos{in.x, in.y};
  if (corner == -1) {
    const PointerPos target = target_point(raw_pointer_);
    in.x = target.x;
    in.y = target.y;
    if (target != raw_pointer_ || pointer_on_target_) {
      in.moved = in.moved || target != previous;
    }
    pointer_on_target_ = target != raw_pointer_;
  }
  if (in.moved) {
    update_hover(in);
  }
  if (!modal_) {
    run_notifies(in);
    run_timers();
  }
  if (in.key_event) {
    if (replaying_ && in.ctrl && in.scan_code == kScanP) {
      show_tutorial_box();
    }
    const bool hotkey = (in.alt || in.scan_code == kScanEsc) && handle_hotkey(in);
    if (!hotkey) {
      broadcast_event(kEventKey, in.ascii, in.scan_code);
    }
  }
  if (in.left_changed && in.left_down) {
    if (exit_corner_ == -1) {
      // Alt + click is the developer's palette pick: nothing otherwise.
      if (!in.alt) {
        broadcast_event(help_mode_ && !modal_ ? kEventHelpClick : kEventLeftClick, in.x, in.y);
      }
    } else if (!modal_) {
      services_.sound.play_sfx(kExitSound);
      set_pointer(kPointerNormal);
      exit_corner_ = -1;
      go_back();
    } else {
      end_modal(-1);
    }
  }
  if (in.right_changed && in.right_down) {
    if (help_mode_) {
      help_mode_ = false;
      services_.set_pointer_frame(kPointerNormal);
    } else {
      broadcast_event(kEventRightClick, in.x, in.y);
    }
  }
  if (in.back) {
    system_back();
  }
  route_view_gesture(in.gesture);
  if (in.wheel != 0) {
    route_wheel(in.x, in.y, in.wheel);
  }
  while (!messages_.empty()) {
    dispatch_messages();
  }
  if (current_ == kScreenStarMap && services_.game.has_game_events() && !modal_) {
    show_game_events();
  }
  // The auto next turn waits for the game events.
  if (auto_turn_ && !services_.game.has_game_events() && !modal_) {
    post_message(kMessageNextTurn, 0, 0);
  }
  pick_music(in);
  SoundSystem& sound = services_.sound;
  do {
    if (sound.music_ready() && sound.tune_playing()) {
      if (sound.music_muted()) {
        sound.stop_tune();
      } else {
        sound.stream_music();
      }
    }
    if (services_.ticks() - last_pass_tick_ < kPassTicks) {
      services_.idle();
    }
  } while (services_.ticks() - last_pass_tick_ < kPassTicks);
  last_pass_tick_ = services_.ticks();
}

void WinMgr::system_back() {
  if (modal_) {
    end_modal(-1);
  } else if (current_ == kScreenOpening) {
    post_message(kMessageQuit, 0, 0);
  } else if (!cancel_action()) {
    services_.sound.play_sfx(kExitSound);
    go_back();
  }
}

PointerPos WinMgr::target_point(PointerPos raw) const {
  if (replaying_ || !enlarged_targets_ || current_ < 0) {
    return raw;
  }
  const auto inside = [](const Pane& pane, PointerPos p) {
    return p.x >= pane.x0 && p.x <= pane.x1 && p.y >= pane.y0 && p.y <= pane.y1;
  };
  const Screen& s = screens_.at(static_cast<std::size_t>(current_));
  std::vector<PressArea> areas;
  const auto receiver = [&](PointerPos p) { return press_receiver(p); };
  if (receiver(raw) != nullptr) {
    return raw;
  }
  const GWindow* found = nullptr;
  Pane target;
  int count = 0;
  for (const int id : s.slots) {
    const GWindow* w = window(id);
    if (w == nullptr || !w->shown()) {
      continue;
    }
    areas.clear();
    w->press_areas(areas);
    for (const PressArea& a : areas) {
      const Pane grown{a.pane.x0 - kTargetMargin, a.pane.y0 - kTargetMargin, a.pane.x1 + kTargetMargin,
                       a.pane.y1 + kTargetMargin};
      if (a.target && inside(grown, raw)) {
        ++count;
        found = w;
        target = a.pane;
      }
    }
  }
  if (count != 1) {
    return raw;
  }
  const PointerPos point{std::clamp(raw.x, target.x0, target.x1), std::clamp(raw.y, target.y0, target.y1)};
  return receiver(point) == found ? point : raw;
}

const GWindow* WinMgr::press_receiver(PointerPos p) const {
  const Screen& s = screens_.at(static_cast<std::size_t>(current_));
  std::vector<PressArea> areas;
  for (auto it = s.slots.rbegin(); it != s.slots.rend(); ++it) {
    const GWindow* w = window(*it);
    if (w == nullptr || !w->shown()) {
      continue;
    }
    areas.clear();
    w->press_areas(areas);
    if (std::ranges::any_of(areas, [&](const PressArea& a) {
          return p.x >= a.pane.x0 && p.x <= a.pane.x1 && p.y >= a.pane.y0 && p.y <= a.pane.y1;
        })) {
      return w;
    }
  }
  return nullptr;
}

LongPress WinMgr::long_press(int x, int y) const {
  if (modal_ || current_ < 0) {
    return LongPress::kHelp;
  }
  const PointerPos p = target_point(PointerPos{x, y});
  const GWindow* w = press_receiver(p);
  return w != nullptr ? w->long_press(p.x, p.y) : LongPress::kHelp;
}

bool WinMgr::cancel_action() {
  if (replaying_ || current_ < 0) {
    return false;
  }
  for (const int id : screens_.at(static_cast<std::size_t>(current_)).slots) {
    if (GWindow* w = window(id); w != nullptr && w->shown() && w->cancel_action()) {
      return true;
    }
  }
  return false;
}

int WinMgr::view_window_at(int x, int y, bool mouse) const {
  if (modal_ || help_mode_ || current_ < 0) {
    return 0;
  }
  const Screen& s = screens_.at(static_cast<std::size_t>(current_));
  for (auto it = s.slots.rbegin(); it != s.slots.rend(); ++it) {
    const GWindow* w = window(*it);
    if (w != nullptr && w->shown() && w->takes_view_gesture(x, y, mouse)) {
      return *it;
    }
  }
  return 0;
}

bool WinMgr::skippable_animation() const {
  if (modal_ || current_ < 0) {
    return false;
  }
  for (const int id : screens_.at(static_cast<std::size_t>(current_)).slots) {
    const GWindow* w = window(id);
    if (w != nullptr && w->shown() && w->plays_skippable_animation()) {
      return true;
    }
  }
  return false;
}

void WinMgr::route_wheel(int x, int y, int notches) {
  if (GWindow* w = window(view_window_at(x, y, false))) {
    w->view_wheel(notches);
  }
}

void WinMgr::route_view_gesture(const ViewGesture& gesture) {
  if (gesture.started) {
    gesture_window_ = view_window_at(gesture.x, gesture.y, gesture.mouse);
  }
  if (GWindow* w = gesture_window_ != 0 && !modal_ ? window(gesture_window_) : nullptr) {
    if (gesture.dx != 0 || gesture.dy != 0) {
      w->view_drag(gesture.dx, gesture.dy);
    }
    if (gesture.zoom != 1.0F) {
      w->view_pinch(gesture.zoom);
    }
  }
  if (!gesture.active) {
    if (GWindow* w = window(gesture_window_)) {
      w->view_end();
    }
    gesture_window_ = 0;
  }
}

void WinMgr::refresh_screen() {
  if (fade_pending_) {
    services_.gsystem.fade_to_palette_step(fade_step_);
    if (++fade_step_ > 64) {
      fade_step_ = 0;
      fade_pending_ = false;
    }
  }
}

void WinMgr::run_pass() {
  process_input(replaying_ ? replay_input() : services_.poll_input());
  refresh_screen();
  services_.present();
  check_replay_abort();
}

void WindowFactory::add(int type, WindowCreator create) { creators_[type] = std::move(create); }

std::unique_ptr<GWindow> WindowFactory::create(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) const {
  if (const auto it = creators_.find(record.type); it != creators_.end()) {
    return it->second(wm, record, shapes);
  }
  return std::make_unique<GWindow>(wm, record, shapes);
}

}  // namespace oa
