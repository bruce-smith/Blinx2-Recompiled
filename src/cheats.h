/**
 * Blinx 2 cheat engine.
 *
 * All cheats are off by default and cost nothing until used. Every function
 * below may be called from any thread (the UI's): it only records a request,
 * which the game thread applies at its next pad poll, between two of the
 * game's own frames' worth of logic. Nothing here races the game's threads.
 *
 * Recording and replay: a cheat applied while input is being recorded is
 * written to the input recording as an event at that poll, and an input
 * replay applies it again at the same poll, so exact replays reproduce
 * cheated runs. During a replay, requests from the UI are ignored (the
 * recording decides) until RECOMP_INPUT_REPLAY_THEN_LIVE hands over.
 *
 * The game state behind each cheat is documented in notes-cheats.md.
 */
#ifndef BLINX2_CHEATS_H
#define BLINX2_CHEATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while a stage or hub is being played (cheats that touch the player or
 * the stage apply), 0 on the title, menus and loading screens. */
int cheats_in_game(void);

/* Teams: 0 Time Sweepers, 1 Tom-Tom Gang, -1 the team being played. */
int  cheats_get_money(int team);              /* -1 if no save is loaded */
void cheats_give_money(int team, int amount); /* negative takes; 0..99999 */

/* Invincibility for the human-controlled characters: hits do no damage
 * and HP is kept at its maximum. */
void cheats_set_invincible(int on);
int  cheats_invincible(void);

/* Infinite ammo: the weapon a human-controlled character holds never runs
 * down (Tom-Tom guns, slingshot, launchers). */
void cheats_set_infinite_ammo(int on);
int  cheats_infinite_ammo(void);

/* Free camera: the camera leaves the player and is flown with the pad or
 * keyboard (left stick move, right stick look, LT/RT down/up, hold A for
 * speed). With freeze_player the game sees no input while it is on. */
void cheats_freecam_enable(int on, int freeze_player);
int  cheats_freecam_enabled(void);

/* Shop and locker items, per team (owned or not). */
int         cheats_item_count(void);
const char *cheats_item_name(int i);
int         cheats_item_owned(int team, int i);   /* -1 if no save loaded */
void        cheats_give_item(int team, int i);
void        cheats_give_all_items(int team);

/* Time controls (Time Sweepers): fill every control type the character can
 * hold to its cap, once or kept full. */
void cheats_fill_time_controls(void);
void cheats_set_infinite_time_controls(int on);
int  cheats_infinite_time_controls(void);

/* Spawn an in-level pickup (item type byte, see notes-cheats.md) just in
 * front of the human player; sub is the type's variant (0 if unsure). */
void cheats_spawn_pickup(int type, int sub);

/* Stage warp. The list is the game's own stage table; warping uses the
 * game's own stage-change routine, as the hub does when a mission starts. */
int         cheats_stage_count(void);
const char *cheats_stage_name(int i);
int         cheats_warp_stage(int i);         /* 0 queued, -1 bad index */

/* Adds the Cheats panel to the in-game menu (main.c, at start). */
void cheats_menu_register(void);

/* ── for recomp_manual.c ── */

/* Before the game's XInputGetState result is recorded or replaced: free cam
 * takes the sticks here, and blanks the pad when the player is frozen. */
void cheats_pre_poll(uint8_t *xinput_state);
/* After it (the game thread, once per pad per frame). */
void cheats_on_poll(uint32_t poll);
/* An input replay's cheat event (22 bytes as recorded). */
void cheats_replay_event(const uint8_t *data);
/* The hit routine's wrapper: 1 = do not hurt this actor. */
int  cheats_block_damage(uint32_t victim);
/* Around the view-matrix build (camera struct address): free camera. */
int  cheats_view_begin(uint32_t cam);
void cheats_view_end(uint32_t cam);

#ifdef __cplusplus
}
#endif

#endif
