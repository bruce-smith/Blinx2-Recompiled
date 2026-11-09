/**
 * Journey Bag automation: drives the game's own Vs. Mode > Prepare for
 * Battle > Create Journey Bag / Return Journey Bag Data with pad presses, so
 * a network game can create a player's bag before the session and return it
 * afterwards without the player walking the menus.
 *
 *   RECOMP_JOURNEYBAG=create:<save slot 1|2>   bag from that home save;
 *                                              fails if a bag is already
 *                                              on the unit, unless
 *                     create:<slot>:overwrite  (which orphans that bag)
 *   RECOMP_JOURNEYBAG=return                   the bag on MU 1A back home,
 *                                              then a "[JBAG] summary:" line
 *                                              (team, gold, items) from the
 *                                              save it went into
 *   RECOMP_JOURNEYBAG=verify:<slot 1|2>        writes nothing: opens Create's
 *                                              home-save list (the game reads
 *                                              and checks every save), checks
 *                                              the slot is there, equal to
 *                                              its file, main record signed,
 *                                              then backs out
 *   RECOMP_JOURNEYBAG=vs                       a real 2-player Vs. game: player
 *                                              1 a saved game, player 2 Load
 *                                              team from Journey Bag > MU 2A
 *                                              (RECOMP_JB_VS_SWAP=1: the other
 *                                              way round), default rules, the
 *                                              match played out; back on team
 *                                              select the game itself saves
 *                                              the bag and the saved game
 *                                              (1-2 s after it gets there).
 *                                              It waits RECOMP_JB_VS_WAIT ms
 *                                              (5000), then each player
 *                                              completes the selection, and
 *                                              "[JBAG] vs ..." lines give the
 *                                              files' times at each step.
 *                                              Driven from the pad filter
 *                                              (usb_gamepad_set_filter), so in
 *                                              a network game each peer drives
 *                                              only its own player;
 *                                              RECOMP_JB_VS_TRACE=1 logs
 *                                              every press
 *   RECOMP_JOURNEYBAG_EXIT=1                   exit when finished:
 *                                              0 done, 5 failed
 *
 * The bag goes to (and comes back from) the memory unit in controller 1
 * slot A (RECOMP_MU, xboxrecomp kernel_mu.c). The game itself picks the
 * records, merges and signs; this only presses buttons, judged from the
 * menu state at each pad poll, so it does not depend on timing. Presses go
 * through the input recording like a player's, and nothing is injected
 * while a recording is being replayed.
 *
 * Log: "[JBAG] done: ..." or "[JBAG] failed: <why>" once.
 */
#ifndef BLINX2_JOURNEYBAG_H
#define BLINX2_JOURNEYBAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At start-up, before the first pad report: reads RECOMP_JOURNEYBAG and,
 * for vs, installs the pad filter (usb_gamepad_set_filter) that drives it. */
void journeybag_init(void);

/* Called from the XInputGetState override with the controller handle and
 * the XINPUT_STATE the title gets, before the input recording sees it. */
void journeybag_pre_poll(uint32_t handle, uint8_t *state);

#ifdef __cplusplus
}
#endif

#endif /* BLINX2_JOURNEYBAG_H */
