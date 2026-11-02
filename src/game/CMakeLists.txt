# The headless game core: tables, state, saves, galaxy generation and the
# day (no SDL).
oa_add_module(oa_game
  SOURCES
    ai.cpp
    antag_ai.cpp
    antag_battle.cpp
    antag_planet.cpp
    battle.cpp
    battle_control.cpp
    cosmos.cpp
    float_math.cpp
    galaxy.cpp
    # peCustom, temporary: the attitude changes with their causes.
    mood_log.cpp
    # The negotiation screen's dialogue (NegWnd's logic).
    negotiation.cpp
    new_game.cpp
    planet.cpp
    # peCustom's planet management by goal (the economy planned ahead).
    planet_plan.cpp
    planet_plan_rules.cpp
    # The player actions of the planet, research and planet-list screens.
    player_actions.cpp
    race.cpp
    race_ai.cpp
    raceneg.cpp
    research.cpp
    save.cpp
    ship.cpp
    # The ship design screen's rules (slot grid, hull cycle, refit cost).
    ship_design.cpp
    ship_gizmo.cpp
    tables.cpp
  DEPS oa_core oa_assets)
