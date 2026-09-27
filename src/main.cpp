// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

// Board bring-up check: echoes DB9 input, reports RTS changes, holds CTS
// asserted. The real firmware replaces this file.

#include "board.h"

void setup() {
    digitalWrite(PIN_CTS_OUT, PIN_ASSERTED);
    pinMode(PIN_CTS_OUT, OUTPUT);
    pinMode(PIN_RTS_IN, INPUT);
    modemBegin(19200);
    Modem.print("\r\nREADY\r\n");
}

void loop() {
    static int rts = -1;
    while (Modem.available()) Modem.write(Modem.read());
    int r = digitalRead(PIN_RTS_IN);
    if (r != rts) {
        rts = r;
        Modem.print(r == PIN_ASSERTED ? "\r\nRTS ON\r\n" : "\r\nRTS OFF\r\n");
    }
}
