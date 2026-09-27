// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

// IP ban table (AT+BAN strikes the current inbound caller): 5 min, then 1 h,
// then 24 h; a caller whose last strike is over a day old starts over.
// Plain C++, no Arduino calls: times are millis() values passed in.

#pragma once

#include <stdint.h>

// ponytail: 256 entries (3 KB of the ESP8266's ~40 KB), stalest reused when full.
#define BAN_MAX 256

void banStrike(uint32_t ip, uint32_t now);
uint32_t banCheck(uint32_t ip, uint32_t now);   // ms of ban left, 0 = let in
void banClear();
int banCount();
// Entry i of the table for AT+BAN?; false past the end. strikes is 0 once
// the last strike is over a day old.
bool banEntry(int i, uint32_t *ip, int *strikes, uint32_t *msLeft, uint32_t now);
