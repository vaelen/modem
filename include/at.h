// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

// Hayes command-line tokenizer. Plain C++, no Arduino calls, so the native
// tests can run it.

#pragma once

#define AT_MAX_CMDS 32

struct AtCmd {
    char prefix;        // 0 = basic letter; '&', '%', '\\', '+', '$', '#'; 'S' = S register
    char name[12];      // uppercased: "E", "K", "BAN", "SSID"; "" for S registers
    int num;            // digits after a basic/&/%/\/S command, -1 if none
    char op;            // 0, '=' or '?'
    const char *arg;    // text after '=' in its original case, NUL-terminated; "" if none
    long val;           // S register value after '=' (0 if no digits), -1 if none
};

// Tokenize an "AT..." line in place. Returns the number of commands, -1 if
// the line doesn't start with AT, -2 if it holds more than AT_MAX_CMDS.
int atParse(char *line, AtCmd *cmds);
