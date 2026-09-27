// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

#include "at.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char up(char c) { return (char)toupper((unsigned char)c); }
static bool digit(char c) { return isdigit((unsigned char)c); }

// Basic commands (letter + digits), S registers (Sn=v / Sn?), prefixed sets
// (&X / %X / \X + digits), extended commands (+NAME[=value][?]), optional
// spaces and semicolons. D, $SSID= and $PASS= take the rest of the line.
int atParse(char *line, AtCmd *cmds) {
    if (up(line[0]) != 'A' || up(line[1]) != 'T') return -1;
    int n = 0;
    char *p = line + 2;
    while (*p) {
        char c = up(*p);
        if (c == ' ' || c == ';') { p++; continue; }
        if (!isalpha((unsigned char)c) && !strchr("&%\\+$#", c)) { p++; continue; }  // junk
        if (n == AT_MAX_CMDS) return -2;
        AtCmd *k = &cmds[n++];
        k->prefix = 0;
        k->name[0] = 0;
        k->num = -1;
        k->op = 0;
        k->arg = "";
        p++;
        if (c == 'D') {                 // dial: T/P modifier, then the rest of the line
            strcpy(k->name, "D");
            if (up(*p) == 'T' || up(*p) == 'P') p++;
            k->arg = p;
            break;
        }
        if (c == '+' || c == '$' || c == '#') {
            int len = 0;
            k->prefix = c;
            while (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
                if (len < (int)sizeof k->name - 1) k->name[len++] = up(*p);
                p++;
            }
            k->name[len] = 0;
            if (*p == '?') {
                k->op = '?';
                p++;
            } else if (*p == '=') {
                k->op = '=';
                k->arg = ++p;
                if (c == '$' && (!strcmp(k->name, "SSID") || !strcmp(k->name, "PASS"))) break;
                while (*p && *p != ';') p++;
                if (*p) *p++ = 0;
            }
            continue;
        }
        if (c == '&' || c == '%' || c == '\\') {
            k->prefix = c;
            if (isalpha((unsigned char)*p)) {
                k->name[0] = up(*p++);
                k->name[1] = 0;
            }
            if (digit(*p)) k->num = (int)strtol(p, &p, 10);
            continue;
        }
        if (c == 'S') {                 // Sn=v / Sn?: value skipped, nothing uses it
            k->prefix = 'S';
            if (digit(*p)) k->num = (int)strtol(p, &p, 10);
            if (*p == '?') {
                k->op = '?';
                p++;
            } else if (*p == '=') {
                k->op = '=';
                p++;
                while (digit(*p)) p++;
            }
            continue;
        }
        k->name[0] = c;                 // basic: letter + digits
        k->name[1] = 0;
        if (digit(*p)) k->num = (int)strtol(p, &p, 10);
    }
    return n;
}
