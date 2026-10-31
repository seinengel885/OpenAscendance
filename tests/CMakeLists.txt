# Unit tests. Tests needing the original game files read the install from the
# OA_DATA environment variable and are skipped when it is unset
# (game_data.h).
add_executable(oa_tests
  # Watcom rand and qsort.
  core_test.cpp
  # The COB archives and the asset decoders.
  assets_test.cpp
  # The text tables.
  tables_test.cpp
  # The translations' tables against the texts of the original's files.
  translation_test.cpp
  # Blitters, fonts and palette fades.
  gfx_test.cpp
  # The mixer and the sound system.
  audio_test.cpp
  # Input polling.
  platform_test.cpp
  # The intro player and the window manager.
  ui_test.cpp
  # The toolkit's window classes.
  widgets_test.cpp
  # The help window and help.txt.
  help_test.cpp
  # The load/save and tutorial screens.
  load_test.cpp
  # The new-game and species history screens.
  newgame_test.cpp
  # The star map and the game session over the game core.
  star_map_test.cpp
  # The planet screen and its square dialog.
  planet_screen_test.cpp
  # The System Display (star-system view and battles).
  star_system_test.cpp
  # The end-of-game screen.
  end_game_test.cpp
  # The research screen.
  research_screen_test.cpp
  # The System Display's effect animations.
  star_system_fx_test.cpp
  # The planet status list.
  status_screen_test.cpp
  # The species selection, negotiation and intelligence screens and the
  # special abilities window.
  diplomacy_screen_test.cpp
  # The ship design screen and the Ships list.
  ship_design_test.cpp
  # The game events, the day and the trace replay through the user
  # interface.
  day_ui_test.cpp
  # The command line and the install choice.
  app_test.cpp
  # The remake's options: the opening menu's button, the options screen,
  # options.cfg.
  options_test.cpp
  # Ship gizmos used in the System Display (oracle runs on edited saves).
  gizmo_battle_test.cpp
  # Ship gizmos used on lanes and planets, the passive gizmos, galaxy moves.
  gizmo_galaxy_test.cpp
  # The species' special abilities.
  ability_test.cpp
  # The game-event codes.
  event_codes_test.cpp
  # The touch screen, the view gestures, Back and the background (remake
  # additions).
  touch_test.cpp
)
target_link_libraries(oa_tests PRIVATE oa_core oa_assets oa_gfx oa_audio oa_platform oa_ui oa_screens oa_game oa_app
  Catch2::Catch2WithMain SDL3::Headers)
include(Catch)
catch_discover_tests(oa_tests)

# The headless game core (src/game): its own executable, without SDL.
# OA_ORACLE names `work/oracle` (the original's reference saves; skipped
# when unset).
add_executable(oa_game_tests
  # Tables, saves and galaxy generation.
  game_test.cpp
  # The day against the oracle corpus.
  day_test.cpp
  # The negotiation screen's dialogue.
  negotiation_test.cpp
  # The peCustom variant and its saves (OA_PECUSTOM_SAVES).
  pecustom_test.cpp
)
target_link_libraries(oa_game_tests PRIVATE oa_game Catch2::Catch2WithMain)
catch_discover_tests(oa_game_tests)
