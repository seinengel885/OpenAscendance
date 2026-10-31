# The translations of the game's texts (`translation.h`), built in: each
# `src/lang/<language>/<file>.tsv` becomes a string of `lang_tables.cpp`.
file(GLOB OA_LANG_TABLES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/lang/*/*.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OA_LANG_TABLES})
set(OA_LANG_ENTRIES "")
foreach(table IN LISTS OA_LANG_TABLES)
  get_filename_component(dir "${table}" DIRECTORY)
  get_filename_component(language "${dir}" NAME)
  get_filename_component(file "${table}" NAME_WE)
  file(READ "${table}" text)
  string(APPEND OA_LANG_ENTRIES "    {\"${language}\", \"${file}\", R\"oa_tsv(${text})oa_tsv\"},\n")
endforeach()
# Written whole rather than configured: the texts may hold `@`.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lang_tables.cpp.new"
  "// Generated from src/lang by src/assets/CMakeLists.txt.\n"
  "#include \"assets/translation.h\"\n\nnamespace oa {\n\nnamespace {\n\n"
  "constexpr TranslationTable kTables[] = {\n    {\"\", \"\", \"\"},\n${OA_LANG_ENTRIES}};\n\n}  // namespace\n\n"
  "std::span<const TranslationTable> built_in_translations() { return std::span(kTables).subspan(1); }\n\n"
  "}  // namespace oa\n")
file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/lang_tables.cpp.new" "${CMAKE_CURRENT_BINARY_DIR}/lang_tables.cpp"
  ONLY_IF_DIFFERENT)

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
    # The built-in translations of the texts.
    translation.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/lang_tables.cpp
    windows_txt.cpp
    # The window-event traces (winmgr.cpp's recorder and replay).
    winevent.cpp
  DEPS oa_core)
