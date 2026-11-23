/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "lap_timer.h"
#define GAME_PILOTS 8
enum { GAME_LAPS=0, GAME_FLAG=1, GAME_QUICK=2, GAME_SOLO=3, GAME_FFA=4, GAME_LOCK=5, GAME_MODE_COUNT=6 };
enum { GAME_IDLE=0, GAME_RUNNING=1, GAME_PAUSED=2, GAME_FINISHED=3 };
typedef struct { uint8_t enabled, channel, team, slot; uint32_t color; char name[17]; } game_pilot_t;
typedef struct {
    uint8_t mode; uint32_t minimum_lap_ms, duration_ms, capture_ms, freshness_ms;
    uint16_t solo_drain_percent,solo_recover_percent;
    game_pilot_t pilots[GAME_PILOTS];
} game_config_t;
typedef struct {
    game_config_t config;
    /* Runtime only; placed before the persisted state tail. */
    uint64_t fault_revision;
    /* Runtime hundredths of a microsecond; terminal results need no fraction. */
    uint8_t life_fraction;
    lap_timer_t laps[GAME_PILOTS];
    uint8_t state, owner, challenger, present_mask, known_mask, fault, winner;
    uint32_t session; uint64_t elapsed_us, progress_us, score_us[2], last_us;
    uint8_t pilot_owner, solo_started, solo_failed;
    uint32_t captures[GAME_PILOTS];
    uint64_t pilot_score_us[GAME_PILOTS], life_us;
    uint64_t seen_us[GAME_PILOTS];
    /* Configuration for this session; config remains the next-match draft. */
    game_config_t match_config;
} game_t;
void game_init(game_t *g);
bool game_config_valid(const game_config_t *c);
bool game_start(game_t *g, uint64_t now, uint8_t online_slots);
bool game_resume(game_t *g, uint64_t now);
void game_tick(game_t *g, uint64_t now);
void game_observe(game_t *g, unsigned pilot, bool present, bool valid, uint64_t now);
void game_pause(game_t *g, uint8_t fault, uint64_t now);
void game_stop(game_t *g, uint64_t now);

static inline const game_config_t *game_session_config(const game_t *g) {
    return g->state==GAME_IDLE ? &g->config : &g->match_config;
}
