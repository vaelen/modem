// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

// Everything board-specific: the serial port wired to the DB9, pins, the WiFi
// header. Port to a new board by adding a block; leave out any pin the board
// doesn't have and the code that uses it compiles out.

#pragma once

#include <Arduino.h>

#if defined(ARDUINO_ESP8266_NODEMCU_ESP12E)     // TheOldNet v6 carrier (see HARDWARE.md)
  #include <ESP8266WiFi.h>
  #define Modem            Serial   // UART0, swapped to GPIO13 (RX) / GPIO15 (TX) → DB9
  #define MODEM_RX_BUFFER  1024     // UART receive buffer; CTS thresholds are ¾ and ¼ of it
  #define PIN_CTS_OUT      12       // D6 → DB9 pin 8
  #define PIN_RTS_IN       14       // D5 ← DB9 pin 7
  #define PIN_ASSERTED     LOW      // MAX3232 inverts: TTL LOW = RS-232 +V (asserted)
  #define PIN_BAUD_RESET   0        // FLASH button, active LOW; LOW at reset = bootloader
  // begin() puts UART0 back on GPIO1/3 (USB); swap() moves it to the DB9.
  inline void modemBegin(uint32_t baud) {
      Serial.setRxBufferSize(MODEM_RX_BUFFER);
      Serial.begin(baud);
      Serial.swap();
  }
#else
  #error "Add your board to include/board.h"
#endif
