// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

#include "bans.h"

#define MINUTE 60000UL
#define DAY (24 * 60 * MINUTE)

struct Ban {
    uint32_t ip;
    uint32_t last;                      // millis() of the last strike
    uint8_t count;                      // strikes so far
};

static Ban bans[BAN_MAX];
static int nbans = 0;

static uint32_t duration(int count) {
    if (count >= 3) return DAY;
    if (count == 2) return 60 * MINUTE;
    return 5 * MINUTE;
}

// Strikes that still count: none once the last one is over a day old.
// Ages are unsigned differences, so millis() wrapping is harmless.
// ponytail: an entry untouched for 49+ days can look fresh again.
static int strikes(const Ban &b, uint32_t now) {
    return now - b.last > DAY ? 0 : b.count;
}

static uint32_t left(const Ban &b, uint32_t now) {
    int s = strikes(b, now);
    uint32_t age = now - b.last;
    if (s == 0 || age >= duration(s)) return 0;
    return duration(s) - age;
}

static Ban *find(uint32_t ip) {
    for (int i = 0; i < nbans; i++)
        if (bans[i].ip == ip) return &bans[i];
    return nullptr;
}

void banStrike(uint32_t ip, uint32_t now) {
    Ban *b = find(ip);
    if (b == nullptr) {
        if (nbans < BAN_MAX) {
            b = &bans[nbans++];
        } else {                        // full: reuse the stalest entry
            b = &bans[0];
            for (int i = 1; i < nbans; i++)
                if (now - bans[i].last > now - b->last) b = &bans[i];
        }
        b->ip = ip;
        b->count = 0;
    }
    b->count = (uint8_t)(strikes(*b, now) < 255 ? strikes(*b, now) + 1 : 255);
    b->last = now;
}

uint32_t banCheck(uint32_t ip, uint32_t now) {
    Ban *b = find(ip);
    return b ? left(*b, now) : 0;
}

void banClear() { nbans = 0; }

int banCount() { return nbans; }

bool banEntry(int i, uint32_t *ip, int *strikesOut, uint32_t *msLeft, uint32_t now) {
    if (i < 0 || i >= nbans) return false;
    *ip = bans[i].ip;
    *strikesOut = strikes(bans[i], now);
    *msLeft = left(bans[i], now);
    return true;
}
