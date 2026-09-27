// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

#include <unity.h>

#include "bans.h"

#define MIN 60000UL
#define HOUR (60 * MIN)
#define DAY (24 * HOUR)

static const uint32_t IP = 0x0100007F;      // 127.0.0.1 as lwIP stores it

void setUp() { banClear(); }
void tearDown() {}

static void unknown_ip_is_let_in() {
    TEST_ASSERT_EQUAL_UINT32(0, banCheck(IP, 1000));
}

static void first_strike_blocks_five_minutes() {
    banStrike(IP, 1000);
    TEST_ASSERT_EQUAL_UINT32(5 * MIN, banCheck(IP, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, banCheck(IP, 1000 + 5 * MIN - 1));
    TEST_ASSERT_EQUAL_UINT32(0, banCheck(IP, 1000 + 5 * MIN));
}

static void strikes_escalate_to_an_hour_then_a_day() {
    banStrike(IP, 0);
    banStrike(IP, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(HOUR, banCheck(IP, 10 * MIN));
    banStrike(IP, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(DAY, banCheck(IP, 2 * HOUR));
    banStrike(IP, 3 * HOUR);            // fourth strike: still a day
    TEST_ASSERT_EQUAL_UINT32(DAY, banCheck(IP, 3 * HOUR));
}

static void a_quiet_day_starts_over() {
    banStrike(IP, 0);
    banStrike(IP, 10 * MIN);
    banStrike(IP, 10 * MIN + DAY + 1);  // over a day since the last: strike 1 again
    TEST_ASSERT_EQUAL_UINT32(5 * MIN, banCheck(IP, 10 * MIN + DAY + 1));
}

static void millis_wrap_keeps_the_ban() {
    uint32_t t = 0xFFFFFF00UL;
    banStrike(IP, t);
    TEST_ASSERT_EQUAL_UINT32(5 * MIN - 0x200, banCheck(IP, t + 0x200));   // wrapped past 0
}

static void clear_empties_the_table() {
    banStrike(IP, 0);
    banClear();
    TEST_ASSERT_EQUAL_INT(0, banCount());
    TEST_ASSERT_EQUAL_UINT32(0, banCheck(IP, 0));
}

static void full_table_reuses_the_stalest_entry() {
    for (uint32_t i = 0; i < BAN_MAX; i++) banStrike(i + 1, 1000 + i);   // ip 1 is stalest
    banStrike(0xABCD, 5000);
    TEST_ASSERT_EQUAL_INT(BAN_MAX, banCount());
    TEST_ASSERT_EQUAL_UINT32(0, banCheck(1, 5000));
    TEST_ASSERT_EQUAL_UINT32(5 * MIN, banCheck(0xABCD, 5000));
    TEST_ASSERT_NOT_EQUAL(0, banCheck(2, 5000));
}

static void entry_reports_ip_strikes_and_time_left() {
    uint32_t ip, msLeft;
    int strikes;
    banStrike(IP, 0);
    banStrike(IP, MIN);
    TEST_ASSERT_TRUE(banEntry(0, &ip, &strikes, &msLeft, 2 * MIN));
    TEST_ASSERT_EQUAL_UINT32(IP, ip);
    TEST_ASSERT_EQUAL_INT(2, strikes);
    TEST_ASSERT_EQUAL_UINT32(HOUR - MIN, msLeft);
    TEST_ASSERT_TRUE(banEntry(0, &ip, &strikes, &msLeft, MIN + DAY + 1));   // stale
    TEST_ASSERT_EQUAL_INT(0, strikes);
    TEST_ASSERT_EQUAL_UINT32(0, msLeft);
    TEST_ASSERT_FALSE(banEntry(1, &ip, &strikes, &msLeft, 0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(unknown_ip_is_let_in);
    RUN_TEST(first_strike_blocks_five_minutes);
    RUN_TEST(strikes_escalate_to_an_hour_then_a_day);
    RUN_TEST(a_quiet_day_starts_over);
    RUN_TEST(millis_wrap_keeps_the_ban);
    RUN_TEST(clear_empties_the_table);
    RUN_TEST(full_table_reuses_the_stalest_entry);
    RUN_TEST(entry_reports_ip_strikes_and_time_left);
    return UNITY_END();
}
