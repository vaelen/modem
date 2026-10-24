// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

#include <string.h>
#include <unity.h>

#include "at.h"

static char line[128];
static AtCmd c[AT_MAX_CMDS];

static int parse(const char *s) {
    strcpy(line, s);
    return atParse(line, c);
}

void setUp() {}
void tearDown() {}

static void non_at_lines_are_ignored() {
    TEST_ASSERT_EQUAL_INT(-1, parse("hello"));
    TEST_ASSERT_EQUAL_INT(-1, parse(""));
    TEST_ASSERT_EQUAL_INT(0, parse("AT"));
    TEST_ASSERT_EQUAL_INT(0, parse("at"));
}

static void basic_commands_take_digits() {
    TEST_ASSERT_EQUAL_INT(3, parse("ATE0V1I"));
    TEST_ASSERT_EQUAL_STRING("E", c[0].name);
    TEST_ASSERT_EQUAL_INT(0, c[0].num);
    TEST_ASSERT_EQUAL_STRING("V", c[1].name);
    TEST_ASSERT_EQUAL_INT(1, c[1].num);
    TEST_ASSERT_EQUAL_STRING("I", c[2].name);
    TEST_ASSERT_EQUAL_INT(-1, c[2].num);
}

static void ampersand_commands() {
    TEST_ASSERT_EQUAL_INT(2, parse("at&k3&W"));
    TEST_ASSERT_EQUAL_INT('&', c[0].prefix);
    TEST_ASSERT_EQUAL_STRING("K", c[0].name);
    TEST_ASSERT_EQUAL_INT(3, c[0].num);
    TEST_ASSERT_EQUAL_STRING("W", c[1].name);
    TEST_ASSERT_EQUAL_INT(1, parse("AT&K?"));
    TEST_ASSERT_EQUAL_STRING("K", c[0].name);
    TEST_ASSERT_EQUAL_INT(-1, c[0].num);
    TEST_ASSERT_EQUAL_INT('?', c[0].op);
}

static void extended_commands_and_chaining() {
    TEST_ASSERT_EQUAL_INT(2, parse("AT+BAN;H"));
    TEST_ASSERT_EQUAL_INT('+', c[0].prefix);
    TEST_ASSERT_EQUAL_STRING("BAN", c[0].name);
    TEST_ASSERT_EQUAL_INT(0, c[0].op);
    TEST_ASSERT_EQUAL_STRING("H", c[1].name);
    TEST_ASSERT_EQUAL_INT(1, parse("at+ban?"));
    TEST_ASSERT_EQUAL_INT('?', c[0].op);
    TEST_ASSERT_EQUAL_INT(2, parse("AT+IPR=38400;E1"));
    TEST_ASSERT_EQUAL_STRING("IPR", c[0].name);
    TEST_ASSERT_EQUAL_STRING("38400", c[0].arg);
    TEST_ASSERT_EQUAL_STRING("E", c[1].name);
}

static void ssid_and_pass_keep_case_and_take_the_rest() {
    TEST_ASSERT_EQUAL_INT(1, parse("at$ssid=My Net; 2.4GHz"));
    TEST_ASSERT_EQUAL_INT('$', c[0].prefix);
    TEST_ASSERT_EQUAL_STRING("SSID", c[0].name);
    TEST_ASSERT_EQUAL_STRING("My Net; 2.4GHz", c[0].arg);
    TEST_ASSERT_EQUAL_INT(1, parse("AT$PASS=pa;SS wOrd"));
    TEST_ASSERT_EQUAL_STRING("pa;SS wOrd", c[0].arg);
    TEST_ASSERT_EQUAL_INT(2, parse("AT$PORT=2323;$SSID?"));
    TEST_ASSERT_EQUAL_STRING("2323", c[0].arg);
    TEST_ASSERT_EQUAL_INT('?', c[1].op);
}

static void dial_takes_the_rest_of_the_line() {
    TEST_ASSERT_EQUAL_INT(2, parse("ATE0DT my-bbs.Example.com:2323"));
    TEST_ASSERT_EQUAL_STRING("D", c[1].name);
    TEST_ASSERT_EQUAL_STRING(" my-bbs.Example.com:2323", c[1].arg);
    TEST_ASSERT_EQUAL_INT(1, parse("atdp10.0.0.1"));
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", c[0].arg);
    TEST_ASSERT_EQUAL_INT(1, parse("ATD"));
    TEST_ASSERT_EQUAL_STRING("", c[0].arg);
}

static void init_strings_parse_cleanly() {
    TEST_ASSERT_EQUAL_INT(8, parse("AT E0 V1 &C1;+GCI=FF;S0=0 X4 %C1 S11?"));
    TEST_ASSERT_EQUAL_INT('S', c[4].prefix);
    TEST_ASSERT_EQUAL_INT(0, c[4].num);
    TEST_ASSERT_EQUAL_INT('=', c[4].op);
    TEST_ASSERT_EQUAL_INT(0, c[4].val);
    TEST_ASSERT_EQUAL_STRING("X", c[5].name);
    TEST_ASSERT_EQUAL_INT(4, c[5].num);
    TEST_ASSERT_EQUAL_INT(0, parse("AT!!;;  "));    // junk only
}

static void s_registers_keep_their_value() {
    TEST_ASSERT_EQUAL_INT(3, parse("ATS0=2X4S11?"));
    TEST_ASSERT_EQUAL_INT(2, c[0].val);
    TEST_ASSERT_EQUAL_STRING("X", c[1].name);
    TEST_ASSERT_EQUAL_INT(11, c[2].num);
    TEST_ASSERT_EQUAL_INT(-1, c[2].val);
    TEST_ASSERT_EQUAL_INT(2, parse("ATS0=A"));        // no digits: 0, as on a Hayes
    TEST_ASSERT_EQUAL_INT(0, c[0].val);
    TEST_ASSERT_EQUAL_STRING("A", c[1].name);
}

static void too_many_commands_is_an_error() {
    char big[80] = "AT";
    for (int i = 0; i <= AT_MAX_CMDS; i++) strcat(big, "E");
    TEST_ASSERT_EQUAL_INT(-2, parse(big));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(non_at_lines_are_ignored);
    RUN_TEST(basic_commands_take_digits);
    RUN_TEST(ampersand_commands);
    RUN_TEST(extended_commands_and_chaining);
    RUN_TEST(ssid_and_pass_keep_case_and_take_the_rest);
    RUN_TEST(dial_takes_the_rest_of_the_line);
    RUN_TEST(init_strings_parse_cleanly);
    RUN_TEST(s_registers_keep_their_value);
    RUN_TEST(too_many_commands_is_an_error);
    return UNITY_END();
}
