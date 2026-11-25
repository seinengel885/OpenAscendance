oa_add_module(oa_assets
  SOURCES
    # The COB archives and loose files (cfile.cpp).
    cfile.cpp
    flc.cpp
    flic_script.cpp
    fnt.cpp
    gif.cpp
    # The screenshot writer's encoder (loadwin.cpp).
    gif_writer.cpp
    # The help files' topics (helpwin.cpp).
    help_txt.cpp
    pal.cpp
    pcm.cpp
    shp.cpp
    sound_tables.cpp
    # static.txt, the string table.
    static_strings.cpp
    # fgets-style line reading of the text tables.
    text_lines.cpp
    tokens.cpp
    # The translations of the texts (`lang/<language>`).
    translation.cpp
    windows_txt.cpp
    # The window-event traces (winmgr.cpp's recorder and replay).
    winevent.cpp
  DEPS oa_core)
