oa_add_module(oa_screens
  SOURCES
    # The System Display: its panel and turns (batcon.cpp), its 3D view
    # (batmode.cpp) and the effect animations (batfx.cpp, batfxa.cpp).
    bat_control_wnd.cpp
    bat_fx.cpp
    bat_mode_wnd.cpp
    # The remake's deselect button on the System Display.
    deselect_button.cpp
    # The 3D camera of the research tree (Camera, vector helpers).
    camera3d.cpp
    # The star map and the Species button (coswnd.cpp).
    cos_wnd.cpp
    # The Ships list (status.cpp).
    fleet_wnd.cpp
    # The intelligence screen and the special abilities window.
    intel_wnd.cpp
    # The load/save and tutorial lists (loadwin.cpp).
    load_wnd.cpp
    # The negotiation screen (NegWnd).
    neg_wnd.cpp
    # The end-of-game screen (eventwnd.cpp).
    end_game_wnd.cpp
    # The new-game set-up (status.cpp).
    new_game_wnd.cpp
    # The remake's Options button and options screen.
    options_wnd.cpp
    # peCustom's management menu of a planet (planet screen, planet list).
    management_menu.cpp
    # The planet screen and its square dialog (planwin.cpp, psqwin.cpp).
    plan_wnd.cpp
    # The research screen (reswin.cpp).
    res_wnd.cpp
    screens.cpp
    # The ship design screen (shipwin.cpp).
    ship_des_wnd.cpp
    # The species history at the start of a game.
    start_game_wnd.cpp
    # The planet status list (status.cpp).
    status_wnd.cpp
    # The System Display's stand-in until the battle screen.
    system_display_stub.cpp
  DEPS oa_core oa_assets oa_gfx oa_audio oa_ui oa_game)
