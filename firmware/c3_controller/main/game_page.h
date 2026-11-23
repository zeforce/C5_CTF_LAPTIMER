#pragma once
#include "game.h"
/* Caller owns the game lock; encodes only requested fields, never copies rings. */
bool game_page_encode(const game_t *game,unsigned page,unsigned offset,uint8_t out[32]);
