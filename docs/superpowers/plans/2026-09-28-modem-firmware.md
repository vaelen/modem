# Modem Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Hayes-style WiFi modem firmware for the TheOldNet v6 board (ESP8266 NodeMCU) that answers TCP callers, dials out, handles `+++`, bans callers by IP and is configured entirely by AT commands on the DB9.

**Architecture:** One PlatformIO project on the Arduino framework. `include/board.h` holds everything board-specific (the `Modem` serial alias, pins, WiFi header). Two plain-C++ modules with no Arduino calls, `at.cpp` (command-line tokenizer) and `bans.cpp` (strike table), are unit-tested natively. `main.cpp` holds the call state machine, command handlers and the profile in EEPROM.

**Tech Stack:** PlatformIO, Arduino core for ESP8266 3.1.2 (`framework-arduinoespressif8266` 3.30102.0), `ESP8266WiFi`, `EEPROM`, Unity (native tests), Python 3 + pyserial (hardware test).

**Spec:** `DESIGN.md` (read it first). Hardware reference: `HARDWARE.md`. Behaviour being carried over: `simple-modem-emulator/modem.c` and its `test.sh`.

## Global Constraints

- DB9 default: 19200 8N1.
- All configuration through AT commands on the DB9. No USB console, web UI, telnet IAC, SLIP or PPP.
- `main.cpp` never names `Serial`, `Serial1` or a GPIO number; those live only in `include/board.h`.
- `at.cpp` and `bans.cpp` use no Arduino headers (they must build under `env:native`).
- Result codes are verbose only: `\r\n<text>\r\n`. Connect message is `CONNECT <baud>`.
- Settings take effect at once but reach flash only on `AT&W`; `AT&F` is RAM-only; `ATZ` reloads flash.
- Ban rules: 5 min, 1 h, then 24 h per strike; a day without strikes resets the count; 256 entries in RAM.
- A new caller whose first 4 bytes are `GET ` gets a strike and is dropped before the host hears of it. Other callers get `CONNECT` after 4 bytes or 1 s, whichever comes first.
- Every source file starts with the two-line header `// Copyright 2026, Andrew C. Young <andrew@vaelen.org>` / `// SPDX-License-Identifier: MIT` (`#` for Python).
- Hardware-in-the-loop steps (flashing, `tools/hwtest.py`) need the board, a USB-RS232 adapter on the DB9 and a WiFi network. An agent without the board does everything else and hands those steps to the user.

## Review Focus

1. **Hyphenated hostnames in `ATDT`** (`my-bbs.example.com`): must dial as typed. `modem.c` stripped dashes; this firmware strips only spaces. Pinned by `test_at` (`dial_takes_the_rest_of_the_line`) in Task 3.
2. **WiFi credentials with lowercase, spaces or `;`**: must be stored exactly as typed. Pinned by `test_at` (`ssid_and_pass_keep_case_and_take_the_rest`) in Task 3 and the `Mixed Case;x` round trip in `hwtest.py config` (Task 4).
3. **Terminal programs that send `ATZ` on startup**: must not drop WiFi. Pinned by the "ATZ must not drop WiFi" check in `hwtest.py config` (Task 4).
4. **A bot hammering from a banned IP**: every attempt dropped silently, the host sees nothing, and the modem still answers other callers. Pinned by the 20-connection loop in `hwtest.py calls` (Task 4).
5. **An over-long command line or credential**: must give `ERROR`, never silently truncate (a truncated password is a baffling WiFi failure). Pinned by the `AT$SSID=`×33, `AT$PASS=`×65 and 200-character line cases in `hwtest.py config` (Task 4).

## File Structure

| File                             | Responsibility                                                      | Task    |
|----------------------------------|---------------------------------------------------------------------|---------|
| `.gitignore`                     | keep `.pio/` build output and flash dumps (`*.bin`) out of git      | 1       |
| `platformio.ini`                 | `env:nodemcuv2` firmware, `env:native` unit tests                   | 1, 2    |
| `include/board.h`                | `Modem` alias, `MODEM_RX_BUFFER`, pins, WiFi header, `modemBegin()` | 1       |
| `include/bans.h`, `src/bans.cpp` | ban table                                                           | 2       |
| `test/test_bans/test_main.cpp`   | ban table unit tests                                                | 2       |
| `include/at.h`, `src/at.cpp`     | AT command-line tokenizer                                           | 3       |
| `test/test_at/test_main.cpp`     | tokenizer unit tests                                                | 3       |
| `tools/hwtest.py`                | end-to-end test on real hardware                                    | 4, 6    |
| `src/main.cpp`                   | bring-up stub (Task 1), then the firmware (Tasks 5, 6)              | 1, 5, 6 |

---

### Task 1: Board layer and DB9 bring-up

Proves the part of the design that only real hardware can: UART0 swapped onto GPIO13/15 reaches the DB9, CTS drives pin 8 and RTS on pin 7 is readable, all with the polarity in `board.h`.

**Files:**
- Create: `.gitignore`, `include/board.h`, `src/main.cpp` (temporary stub)
- Modify: `platformio.ini` (whole file)

**Interfaces:**
- Produces: `Modem` (macro for the serial port object; supports `begin`/`available`/`read`/`readBytes`/`write`/`print`/`flush`/`availableForWrite`), `MODEM_RX_BUFFER` (int, 1024), `PIN_CTS_OUT` (12), `PIN_RTS_IN` (14), `PIN_ASSERTED` (`LOW`), `PIN_BAUD_RESET` (0), `void modemBegin(uint32_t baud)`. Including `board.h` also brings in `<Arduino.h>` and `<ESP8266WiFi.h>`.

- [ ] **Step 1: Start the git repository**

The project directory isn't a git repository yet.

```bash
cd /home/andrew/repos/modem
git init
```

Create `.gitignore`:

```
.pio/
*.bin
```

`*.bin` matters: `HARDWARE.md` warns that a stock flash dump contains the WiFi password.

- [ ] **Step 2: Write `platformio.ini`**

Replace the whole file:

```ini
; PlatformIO Project Configuration File
;
;   Build options: build flags, source filter
;   Upload options: custom upload port, speed and extra flags
;   Library options: dependencies, extra library storages
;   Advanced options: extra scripting
;
; Please visit documentation for the other options and examples
; https://docs.platformio.org/page/projectconf.html

[platformio]
default_envs = nodemcuv2

[env:nodemcuv2]
platform = espressif8266
board = nodemcuv2
framework = arduino
build_flags = -Wall -Wextra

```

`default_envs` keeps a bare `pio run` from also building `env:native` (added in Task 2), which has no `main()`.

- [ ] **Step 3: Write `include/board.h`**

```cpp
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
```

- [ ] **Step 4: Write the bring-up stub `src/main.cpp`**

```cpp
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
```

- [ ] **Step 5: Build**

Run: `pio run`
Expected: `[SUCCESS]`, no warnings from `src/` or `include/`.

- [ ] **Step 6: Flash and check on hardware** (hardware-in-the-loop)

Back up the stock firmware first if you haven't (`HARDWARE.md` §1): `esptool.py --port /dev/ttyUSB0 read_flash 0 ALL v6-stock.bin`.

Flash over the NodeMCU's USB port: `pio run -t upload`.

Then, with a USB-RS232 adapter on the DB9 (`/dev/ttyUSB1` here; adjust):

```bash
pip install pyserial
python3 - <<'EOF'
import serial, time
s = serial.Serial("/dev/ttyUSB1", 19200, timeout=2)
s.rts = True
time.sleep(0.3)
s.reset_input_buffer()
s.write(b"hello")
print("echo:", s.read(5))                # b'hello'
print("CTS asserted:", s.cts)            # True
s.rts = False
print(s.read(11))                        # b'\r\nRTS OFF\r\n'
s.rts = True
print(s.read(10))                        # b'\r\nRTS ON\r\n'
EOF
```

Expected: the four commented values. Pressing RST on the board prints `READY` on the DB9 port.
If the echo fails, the swap or the TX/RX pins are wrong. If `CTS asserted` is `False` or the RTS lines are the other way round, `PIN_ASSERTED` is wrong. Fix `board.h` and repeat before going further.

- [ ] **Step 7: Commit**

```bash
git add .gitignore platformio.ini include/board.h src/main.cpp DESIGN.md HARDWARE.md docs/
git commit -m "Add board layer and DB9 bring-up stub"
```

---

### Task 2: Ban table

**Files:**
- Create: `include/bans.h`, `src/bans.cpp`, `test/test_bans/test_main.cpp`
- Modify: `platformio.ini` (append `env:native`)

**Interfaces:**
- Produces (all times are `millis()` values, IPs are `uint32_t` as `IPAddress` converts them):
  - `#define BAN_MAX 256`
  - `void banStrike(uint32_t ip, uint32_t now);`
  - `uint32_t banCheck(uint32_t ip, uint32_t now);` ms of ban left, 0 = let in
  - `void banClear();`
  - `int banCount();`
  - `bool banEntry(int i, uint32_t *ip, int *strikes, uint32_t *msLeft, uint32_t now);` false past the end; `strikes` is 0 once the last strike is over a day old

- [ ] **Step 1: Add the native test environment**

Append to `platformio.ini`:

```ini

; Unit tests for the Arduino-free modules: pio test -e native
[env:native]
platform = native
test_build_src = yes
build_src_filter = +<bans.cpp> +<at.cpp>
```

`test_build_src = yes` links the listed `src/` files into the test program; the filter keeps `main.cpp` (Arduino-only) out.

- [ ] **Step 2: Write the header `include/bans.h`**

```cpp
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
```

- [ ] **Step 3: Write the failing test `test/test_bans/test_main.cpp`**

```cpp
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
```

- [ ] **Step 4: Run it to see it fail**

Run: `pio test -e native -f test_bans`
Expected: link errors, `undefined reference to 'banClear()'` (and the other functions). The first run also downloads the `native` platform.

- [ ] **Step 5: Implement `src/bans.cpp`**

```cpp
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
```

- [ ] **Step 6: Run the test to see it pass**

Run: `pio test -e native -f test_bans`
Expected: `8 test cases: 8 succeeded`.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini include/bans.h src/bans.cpp test/test_bans
git commit -m "Add IP ban table with native tests"
```

---

### Task 3: AT command-line tokenizer

**Files:**
- Create: `include/at.h`, `src/at.cpp`, `test/test_at/test_main.cpp`

**Interfaces:**
- Produces:
  - `#define AT_MAX_CMDS 32`
  - `struct AtCmd { char prefix; char name[12]; int num; char op; const char *arg; };` (field meanings in the header comments)
  - `int atParse(char *line, AtCmd *cmds);` returns the command count, -1 if the line doesn't start with `AT`, -2 if more than `AT_MAX_CMDS`. Writes NULs into `line`; `arg` points into it.

- [ ] **Step 1: Write the header `include/at.h`**

```cpp
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
};

// Tokenize an "AT..." line in place. Returns the number of commands, -1 if
// the line doesn't start with AT, -2 if it holds more than AT_MAX_CMDS.
int atParse(char *line, AtCmd *cmds);
```

- [ ] **Step 2: Write the failing test `test/test_at/test_main.cpp`**

```cpp
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
    TEST_ASSERT_EQUAL_STRING("X", c[5].name);
    TEST_ASSERT_EQUAL_INT(4, c[5].num);
    TEST_ASSERT_EQUAL_INT(0, parse("AT!!;;  "));    // junk only
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
    RUN_TEST(too_many_commands_is_an_error);
    return UNITY_END();
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `pio test -e native -f test_at`
Expected: link error, `undefined reference to 'atParse(char*, AtCmd*)'`.

- [ ] **Step 4: Implement `src/at.cpp`**

```cpp
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
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `pio test -e native`
Expected: `16 test cases: 16 succeeded` (8 in `test_at`, 8 in `test_bans`).

- [ ] **Step 6: Commit**

```bash
git add include/at.h src/at.cpp test/test_at
git commit -m "Add AT command tokenizer with native tests"
```

---

### Task 4: Hardware test script (config and calls)

The firmware's end-to-end tests, written before the firmware. It's a port of `simple-modem-emulator/test.sh` to real hardware, plus the configuration commands. The `flow` section is added in Task 6.

**Files:**
- Create: `tools/hwtest.py`

**Interfaces:**
- Consumes: the AT command set and result strings in `DESIGN.md` §AT commands, exactly as Task 5 implements them: `OK`, `ERROR`, `CONNECT 19200`, `NO CARRIER`, `BUSY, PLEASE TRY AGAIN LATER\r\n`, info lines `MODEM 1.0`, `WIFI: CONNECTED <ssid> <ip>` / `WIFI: DISCONNECTED <ssid>` / `WIFI: NOT CONFIGURED`, `PORT: n`, `BAUD: n`, `BANS: n`, `+IPR: n`, `$SSID: s`, `$PORT: n`, `+BAN: ip,strikes,minutes`.
- Produces: `tools/hwtest.py --port DEV --modem IP --me IP [config] [calls]`, with helpers `rd`, `rd_until`, `at`, `srd`, `eof`, `eq`, `listener`, `escape`, `wait_wifi`, `value` that Task 6 reuses.

- [ ] **Step 1: Write `tools/hwtest.py`**

```python
#!/usr/bin/env python3
# Copyright 2026, Andrew C. Young <andrew@vaelen.org>
# SPDX-License-Identifier: MIT
"""End-to-end test against the real modem: pyserial drives the DB9 through a
USB-RS232 adapter, sockets on this machine play the callers and dialed peers.

Before running: the modem has WiFi configured and saved (AT$SSID=, AT$PASS=,
AT&W), is at 19200 baud with &K0 saved, and can reach this machine on
PEER_PORT. Sections run in order; a failed assert stops the run.

  pip install pyserial
  tools/hwtest.py --port /dev/ttyUSB0 --modem 192.168.1.50 --me 192.168.1.10 [config] [calls]
"""
import argparse
import socket
import time

import serial

OK = b"\r\nOK\r\n"
ERROR = b"\r\nERROR\r\n"
CONNECT = b"\r\nCONNECT 19200\r\n"
NOCARRIER = b"\r\nNO CARRIER\r\n"
BUSY = b"BUSY, PLEASE TRY AGAIN LATER\r\n"
PEER_PORT = 2324


def rd(ser, t=2.0):
    """DB9 bytes: waits up to t for the first, then until 0.3 s of quiet."""
    ser.timeout = t
    got = ser.read(1)
    if not got:
        return b""
    ser.timeout = 0.3
    while True:
        more = ser.read(4096)
        if not more:
            return got
        got += more


def rd_until(ser, want, t=10.0):
    got, end = b"", time.time() + t
    while want not in got and time.time() < end:
        got += rd(ser, 0.5)
    return got


def at(ser, cmd, t=2.0):
    ser.write(cmd + b"\r")
    return rd(ser, t)


def srd(s, t=2.0):
    """Socket bytes: waits up to t for the first, then until 0.3 s of quiet."""
    s.settimeout(t)
    try:
        got = s.recv(4096)
    except socket.timeout:
        return b""
    s.settimeout(0.3)
    while got:
        try:
            more = s.recv(4096)
        except socket.timeout:
            break
        if not more:
            break
        got += more
    return got


def eof(s, t=3.0):
    """True if the other end closed (EOF or reset) within t."""
    s.settimeout(t)
    try:
        return s.recv(4096) == b""
    except ConnectionResetError:
        return True
    except socket.timeout:
        return False


def eq(got, want):
    assert got == want, f"got {got!r}, want {want!r}"


def listener(me, rcvbuf=None):
    l = socket.socket()
    l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if rcvbuf:
        l.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)   # inherited by accept()
    l.bind((me, PEER_PORT))
    l.listen(1)
    l.settimeout(15)
    return l


def escape(ser):
    time.sleep(1.1)
    ser.write(b"+++")
    eq(rd(ser), OK)


def wait_wifi(ser):
    end = time.time() + 30
    while time.time() < end:
        if b"WIFI: CONNECTED" in at(ser, b"ATI"):
            return
        time.sleep(1)
    raise AssertionError("WiFi did not reconnect")


def value(ser, query):
    """'AT$PORT?' -> b'23'"""
    got = at(ser, query)
    assert got.endswith(OK), got
    return got.split(b"\r\n")[1].split(b": ", 1)[1]


def test_config(ser, a):
    at(ser, b"ATZ")
    at(ser, b"ATE0")
    eq(at(ser, b"AT"), OK)
    eq(at(ser, b"at"), OK)
    eq(at(ser, b"hello"), b"")                          # not a command: no answer
    # echo, backspace, and a line applied only after OK (ATE0 is echoed)
    eq(at(ser, b"ATE1"), OK)
    eq(at(ser, b"AX\bT"), b"AX\b \bT\r" + OK)
    eq(at(ser, b"ATE0"), b"ATE0\r" + OK)
    # init strings and unknown commands are OK
    eq(at(ser, b"AT E0 V1 &C1;+GCI=FF;S0=0 X4 %C1 S11?"), OK)
    # errors, and an ERROR line changes nothing
    port = value(ser, b"AT$PORT?")
    for bad in (b"ATE2", b"AT&K1", b"AT+IPR=12345", b"AT$PASS?", b"AT$PORT=0",
                b"AT$PORT=70000", b"AT$SSID=" + b"x" * 33, b"AT$PASS=" + b"x" * 65,
                b"AT+BAN", b"AT+BAN=1", b"AT" + b"E" * 200):
        eq(at(ser, bad), ERROR)
    eq(at(ser, b"AT$PORT=2323;E2"), ERROR)
    eq(value(ser, b"AT$PORT?"), port)
    # ATZ (in many terminal init strings) must not drop WiFi
    eq(at(ser, b"ATZ"), OK)
    at(ser, b"ATE0")
    assert b"WIFI: CONNECTED" in at(ser, b"ATI"), "ATZ dropped WiFi"
    # ATI
    got = at(ser, b"ATI")
    for want in (b"MODEM 1.0", b"WIFI: CONNECTED", b"BAUD: 19200", b"BANS: "):
        assert want in got, got
    assert got.endswith(OK) and b"PASS" not in got, got
    # SSID keeps case and ';'; ATZ undoes an unsaved change and WiFi comes back
    ssid = value(ser, b"AT$SSID?")
    eq(at(ser, b"AT$SSID=Mixed Case;x"), OK)
    eq(value(ser, b"AT$SSID?"), b"Mixed Case;x")
    eq(at(ser, b"ATZ"), OK)
    eq(value(ser, b"AT$SSID?"), ssid)
    wait_wifi(ser)
    # AT&F is RAM only: defaults (echo on, WiFi cleared), ATZ brings the profile back
    eq(at(ser, b"AT&F"), OK)
    eq(at(ser, b"ATE0"), b"ATE0\r" + OK)
    assert b"WIFI: NOT CONFIGURED" in at(ser, b"ATI")
    eq(at(ser, b"ATZ"), OK)
    wait_wifi(ser)
    # AT&W persists (ATZ reloads flash); without it ATZ reverts
    eq(at(ser, b"AT$PORT=2400"), OK)
    eq(at(ser, b"ATZ"), OK)
    eq(value(ser, b"AT$PORT?"), port)
    eq(at(ser, b"AT$PORT=2400&W"), OK)
    eq(at(ser, b"ATZ"), OK)
    eq(value(ser, b"AT$PORT?"), b"2400")
    eq(at(ser, b"AT$PORT=" + port + b"&W"), OK)
    # +IPR: OK at the old rate, then the new one; ATZ goes back to the saved 19200
    eq(at(ser, b"AT+IPR=38400"), OK)
    ser.baudrate = 38400
    eq(at(ser, b"AT"), OK)
    eq(value(ser, b"AT+IPR?"), b"38400")
    eq(at(ser, b"ATZ"), OK)
    ser.baudrate = 19200
    eq(at(ser, b"AT"), OK)


def test_calls(ser, a):
    at(ser, b"ATZ")
    at(ser, b"ATE0")
    port = int(value(ser, b"AT$PORT?"))

    def call(p=port):
        return socket.create_connection((a.modem, p), timeout=5)

    # inbound: CONNECT, data both ways
    caller = call()
    eq(rd(ser), CONNECT)
    caller.sendall(b"hello")
    eq(rd(ser), b"hello")
    ser.write(b"world")
    eq(srd(caller), b"world")
    # "+++" without guard time is data
    ser.write(b"a+++b")
    eq(srd(caller), b"a+++b")
    # escape: OK, not forwarded; ATO back to data
    escape(ser)
    eq(srd(caller, 1), b"")
    eq(at(ser, b"ATO"), CONNECT)
    ser.write(b"more")
    eq(srd(caller), b"more")
    # remote data while in command mode is dropped
    escape(ser)
    caller.sendall(b"lost")
    time.sleep(0.5)
    eq(at(ser, b"ATO"), CONNECT)
    eq(rd(ser, 1), b"")
    # ATH0 hangs up; the caller is dropped; command mode still works
    escape(ser)
    eq(at(ser, b"ATH0"), NOCARRIER)
    assert eof(caller)
    caller.close()
    eq(at(ser, b"AT"), OK)
    eq(at(ser, b"ATH"), OK)                             # already on-hook
    # caller hangs up
    caller = call()
    eq(rd(ser), CONNECT)
    caller.close()
    eq(rd(ser, 5), NOCARRIER)
    # dial out (spaces ignored), data both ways
    peer_l = listener(a.me)
    ser.write(b"ATDT " + " ".join(f"{a.me}:{PEER_PORT}").encode() + b"\r")
    peer, _ = peer_l.accept()
    eq(rd(ser, 12), CONNECT)
    ser.write(b"out")
    eq(srd(peer), b"out")
    peer.sendall(b"in")
    eq(rd(ser), b"in")
    # a caller during a call is BUSY; ATD while online is ERROR
    c = call()
    eq(srd(c), BUSY)
    c.close()
    escape(ser)
    eq(at(ser, b"ATDT1.2.3.4"), ERROR)
    eq(at(ser, b"ATO"), CONNECT)
    # the dialed side hangs up
    peer.close()
    eq(rd(ser, 5), NOCARRIER)
    # unknown number, refused port, bad port
    eq(at(ser, b"ATDT555"), NOCARRIER)
    eq(at(ser, f"ATDT{a.me}:1".encode(), 12), NOCARRIER)
    eq(at(ser, f"ATDT{a.me}:99999".encode()), NOCARRIER)
    # hanging up closes the peer
    ser.write(f"ATDT{a.me}:{PEER_PORT}\r".encode())
    peer, _ = peer_l.accept()
    eq(rd(ser, 12), CONNECT)
    escape(ser)
    eq(at(ser, b"ATH"), NOCARRIER)
    assert eof(peer)
    peer.close()
    # a peer flooding faster than 19200 must not swallow the escape
    ser.write(f"ATDT{a.me}:{PEER_PORT}\r".encode())
    peer, _ = peer_l.accept()
    eq(rd(ser, 12), CONNECT)
    peer.setblocking(False)
    end = time.time() + 2
    while time.time() < end:                           # ~2 s of flood, DB9 unread
        try:
            peer.send(b"*" * 4096)
        except BlockingIOError:
            time.sleep(0.01)
    time.sleep(1.1)
    ser.write(b"+++")
    time.sleep(1.1)
    ser.write(b"ATH\r")
    assert NOCARRIER in rd_until(ser, NOCARRIER), "escape lost under flood"
    peer.close()
    peer_l.close()
    eq(at(ser, b"AT"), OK)
    # AT+BAN;H bans and hangs up; the banned IP is dropped silently
    caller = call()
    eq(rd(ser), CONNECT)
    escape(ser)
    eq(at(ser, b"AT+BAN;H"), NOCARRIER)
    caller.close()
    for _ in range(20):                                 # a bot hammering: every try dropped
        c = call()
        assert eof(c)                                   # no BUSY text, just closed
        c.close()
    eq(rd(ser, 1), b"")                                 # the host never saw any of it
    got = at(ser, b"AT+BAN?")
    assert f"+BAN: {a.me},1,5\r\n".encode() in got and got.endswith(OK), got
    eq(at(ser, b"AT+BAN=0"), OK)
    eq(at(ser, b"AT+BAN?"), OK)
    caller = call()
    eq(rd(ser), CONNECT)
    caller.close()
    eq(rd(ser, 5), NOCARRIER)
    # an HTTP scanner ("GET " first) is banned and dropped; the host never sees it
    c = call()
    c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    assert eof(c)
    c.close()
    eq(rd(ser, 2), b"")
    assert f"+BAN: {a.me},1,5\r\n".encode() in at(ser, b"AT+BAN?")
    eq(at(ser, b"AT+BAN=0"), OK)
    # other first bytes, even ones that start like GET, are answered and delivered
    c = call()
    c.sendall(b"GETX")
    eq(rd(ser), CONNECT + b"GETX")
    c.close()
    eq(rd(ser, 5), NOCARRIER)
    c = call()
    c.sendall(b"hi")                                    # under 4 bytes: answered after 1 s
    eq(rd(ser), CONNECT + b"hi")
    c.close()
    eq(rd(ser, 5), NOCARRIER)
    # a caller that hangs up before it's answered is dropped silently
    c = call()
    c.close()
    eq(rd(ser, 2), b"")
    # AT$PORT moves the listener at once; ATZ moves it back
    eq(at(ser, b"AT$PORT=2323"), OK)
    caller = call(2323)
    eq(rd(ser), CONNECT)
    caller.close()
    eq(rd(ser, 5), NOCARRIER)
    eq(at(ser, b"ATZ"), OK)
    at(ser, b"ATE0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial device wired to the DB9")
    ap.add_argument("--modem", required=True, help="the modem's IP (ATI)")
    ap.add_argument("--me", required=True, help="this machine's IP, reachable from the modem")
    ap.add_argument("sections", nargs="*", default=["config", "calls"])
    a = ap.parse_args()
    ser = serial.Serial(a.port, 19200, timeout=2)
    ser.rts = True
    time.sleep(0.2)
    ser.reset_input_buffer()
    for name in a.sections:
        globals()["test_" + name](ser, a)
        print(name, "ok")


if __name__ == "__main__":
    main()
```

```bash
chmod +x tools/hwtest.py
python3 -m py_compile tools/hwtest.py
```

- [ ] **Step 2: Run it against the bring-up stub to see it fail** (hardware-in-the-loop)

Run: `tools/hwtest.py --port /dev/ttyUSB1 --modem 192.168.1.50 --me 192.168.1.10 config` (any IPs; the stub has no WiFi)
Expected: `AssertionError: got b'AT\r', want b'\r\nOK\r\n'` in `test_config`. The stub echoes input instead of answering it.

- [ ] **Step 3: Commit**

```bash
git add tools/hwtest.py
git commit -m "Add hardware end-to-end test for config and calls"
```

---

### Task 5: Firmware: command mode, profile, calls and bans

Replaces the stub with the firmware. RTS/CTS flow control is left to Task 6: here `&K` is accepted and stored but has no effect, and CTS stays asserted.

**Files:**
- Modify: `src/main.cpp` (replace the whole file)

**Interfaces:**
- Consumes: `board.h` (Task 1), `bans.h` (Task 2), `at.h` (Task 3).
- Produces for Task 6: `static bool hostReady()` (true = may send to the host; called by `pumpToModem()`), `static Config cfg` with `cfg.flow` (0 or 3), and `loop()` ending with the hang-up check.

Notes for the implementer:
- `runLine()` builds the new profile in `next` and applies it only after printing `OK`. This is what sends `+IPR`'s `OK` at the old baud, and it makes an `ERROR` line change nothing.
- `apply()` only touches what changed, so `ATZ` with an unchanged profile doesn't reconnect WiFi.
- `pumpToTcp()` reads no more from the UART than the TCP send buffer can take, and `pumpToModem()` no more from TCP than the UART can take. Unread data waits (UART buffer or lwIP), which is the backpressure. Don't add a queue.
- `dial()` blocks for up to 10 s. That's deliberate (see the `ponytail:` comment).
- A new caller sits in `ANSWERING` until `checkAnswer()` decides: `GET ` → strike and drop, hung up → drop, otherwise `CONNECT` plus the held bytes. The host never hears about a dropped caller, so there's no `NO CARRIER` for it.

- [ ] **Step 1: Write `src/main.cpp`**

```cpp
// Copyright 2026, Andrew C. Young <andrew@vaelen.org>
// SPDX-License-Identifier: MIT

// Hayes-style WiFi modem: answers TCP callers, dials out to host[:port],
// +++ escape, AT+BAN IP bans, configuration by AT commands. See DESIGN.md.

#include <EEPROM.h>
#include <stdarg.h>

#include "at.h"
#include "bans.h"
#include "board.h"

#define VERSION "1.0"
#define GUARD_MS 500            // +++ guard time (Hayes S12 is 1 s)
#define DIAL_TIMEOUT_MS 10000   // DNS + TCP connect
#define BAUD_RESET_MS 3000      // FLASH held this long → 19200
#define ANSWER_MS 1000          // wait this long for a new caller's first 4 bytes
#define CONFIG_MAGIC 0x314D444DUL   // "MDM1" little-endian

struct Config {
    uint32_t magic;
    char ssid[33];
    char pass[65];
    uint32_t baud;
    uint16_t port;
    uint8_t echo;
    uint8_t flow;               // 0 = none, 3 = RTS/CTS
};

enum State { IDLE, ANSWERING, ONLINE, ONLINE_CMD };  // on-hook, caller not yet announced, data, after +++

static const uint32_t BAUDS[] = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };

static Config cfg;                      // the live profile; flash only via AT&W
static State state = IDLE;
static WiFiServer server(23);
static WiFiClient client;               // the call
static uint32_t callerIp = 0;           // inbound caller, 0 = none (outbound or idle)
static char line[128];                  // command line being typed
static int nline = 0;
static bool lineOverflow = false;
static bool infoOpen = false;           // an info block has started this command line
static int nheld = 0;                   // '+' bytes held back pending the guard time
static uint32_t lastModemMs = 0;        // when the last byte came from the host
static uint8_t early[4];                // an answering caller's first bytes
static int nearly = 0;
static uint32_t answerMs = 0;           // when the answering caller connected

static void defaults(Config &c) {
    memset(&c, 0, sizeof c);
    c.magic = CONFIG_MAGIC;
    c.baud = 19200;
    c.port = 23;
    c.echo = 1;
}

static bool validBaud(uint32_t b) {
    for (uint32_t v : BAUDS)
        if (v == b) return true;
    return false;
}

static void loadProfile(Config &c) {
    EEPROM.get(0, c);
    if (c.magic != CONFIG_MAGIC || !validBaud(c.baud) || c.port == 0) defaults(c);
    c.ssid[sizeof c.ssid - 1] = 0;
    c.pass[sizeof c.pass - 1] = 0;
}

static void wifiBegin() {
    if (cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass);
    else WiFi.disconnect();
}

// Make n the live profile, touching only what changed: ATZ is in many
// terminal init strings and must not drop WiFi.
static void apply(const Config &n) {
    Config old = cfg;
    cfg = n;
    if (cfg.baud != old.baud) {
        Modem.flush();
        modemBegin(cfg.baud);
    }
    if (strcmp(cfg.ssid, old.ssid) || strcmp(cfg.pass, old.pass)) wifiBegin();
    if (cfg.port != old.port) {
        server.stop();
        server.begin(cfg.port);
    }
}

static void result(const char *s) {
    Modem.print("\r\n");
    Modem.print(s);
    Modem.print("\r\n");
}

// One line of an information response ("\r\n" once, then line + "\r\n").
static void info(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!infoOpen) Modem.print("\r\n");
    infoOpen = true;
    Modem.print(buf);
    Modem.print("\r\n");
}

static void goOnline() {
    char buf[24];
    state = ONLINE;
    nheld = 0;
    lastModemMs = millis();
    snprintf(buf, sizeof buf, "CONNECT %lu", (unsigned long)cfg.baud);
    result(buf);
}

// The call is over (remote gone or ATH): NO CARRIER, on-hook.
static void hangup() {
    client.stop();
    callerIp = 0;
    nheld = 0;
    nline = 0;
    state = IDLE;
    result("NO CARRIER");
}

// ATD: "host[:port]", spaces ignored, port 23 by default. Blocks for up to
// DIAL_TIMEOUT_MS. ponytail: no keypress abort; async connect if it matters.
static void dial(const char *s) {
    char host[100];
    int n = 0;
    uint16_t port = 23;
    for (; *s && n < (int)sizeof host - 1; s++)
        if (*s != ' ') host[n++] = *s;
    host[n] = 0;
    char *colon = strrchr(host, ':');
    if (colon) {
        long p = atol(colon + 1);
        *colon = 0;
        if (p < 1 || p > 65535) { result("NO CARRIER"); return; }
        port = (uint16_t)p;
    }
    if (!host[0] || (!colon && !strchr(host, '.'))) { result("NO CARRIER"); return; }
    client.setTimeout(DIAL_TIMEOUT_MS);
    if (!client.connect(host, port)) {
        client.stop();
        result("NO CARRIER");
        return;
    }
    client.setNoDelay(true);
    callerIp = 0;
    goOnline();
}

static void showInfo() {
    info("MODEM " VERSION);
    if (!cfg.ssid[0]) info("WIFI: NOT CONFIGURED");
    else if (WiFi.status() == WL_CONNECTED)
        info("WIFI: CONNECTED %s %s", cfg.ssid, WiFi.localIP().toString().c_str());
    else info("WIFI: DISCONNECTED %s", cfg.ssid);
    info("PORT: %u", cfg.port);
    info("BAUD: %lu", (unsigned long)cfg.baud);
    info("BANS: %d", banCount());
}

static void listBans() {
    uint32_t ip, msLeft, now = millis();
    int strikes;
    for (int i = 0; banEntry(i, &ip, &strikes, &msLeft, now); i++)
        info("+BAN: %s,%d,%lu", IPAddress(ip).toString().c_str(), strikes,
             (unsigned long)((msLeft + 59999) / 60000));
}

// Run one command line. Setting commands change `next`; the whole line is
// applied after OK (so +IPR's OK goes out at the old rate) or dropped on ERROR.
static void runLine() {
    AtCmd cmds[AT_MAX_CMDS];
    int n = atParse(line, cmds);
    if (n == -1) return;                // not a command: ignored, like modem.c
    if (n < 0) { result("ERROR"); return; }
    Config next = cfg;
    bool err = false, save = false;
    infoOpen = false;
    for (int i = 0; i < n && !err; i++) {
        AtCmd &c = cmds[i];
        int v = c.num < 0 ? 0 : c.num;
        if (c.prefix == 0) {
            switch (c.name[0]) {
            case 'D':
                if (state != IDLE) { err = true; break; }
                apply(next);
                dial(c.arg);            // prints its own result
                return;
            case 'H':
                if (state == ONLINE_CMD) { apply(next); hangup(); return; }
                break;
            case 'O':
                if (state == ONLINE_CMD) { apply(next); goOnline(); return; }
                break;
            case 'E':
                if (v > 1) err = true;
                else next.echo = (uint8_t)v;
                break;
            case 'I': showInfo(); break;
            case 'Z':
                if (state == ONLINE_CMD) hangup();
                loadProfile(next);
                break;
            }
        } else if (c.prefix == '&') {
            if (c.name[0] == 'K') {
                if (v == 0 || v == 3) next.flow = (uint8_t)v;
                else err = true;
            } else if (c.name[0] == 'W') {
                save = true;
            } else if (c.name[0] == 'F') {
                defaults(next);
            }
        } else if (c.prefix == '+' && !strcmp(c.name, "BAN")) {
            if (c.op == '?') listBans();
            else if (c.op == '=' && !strcmp(c.arg, "0")) banClear();
            else if (c.op == 0 && state == ONLINE_CMD && callerIp != 0) banStrike(callerIp, millis());
            else err = true;
        } else if (c.prefix == '+' && !strcmp(c.name, "IPR")) {
            if (c.op == '?') info("+IPR: %lu", (unsigned long)next.baud);
            else if (c.op == '=' && validBaud(strtoul(c.arg, nullptr, 10))) next.baud = strtoul(c.arg, nullptr, 10);
            else err = true;
        } else if (c.prefix == '$' && !strcmp(c.name, "SSID")) {
            if (c.op == '?') info("$SSID: %s", next.ssid);
            else if (c.op == '=' && strlen(c.arg) < sizeof next.ssid) strcpy(next.ssid, c.arg);
            else err = true;
        } else if (c.prefix == '$' && !strcmp(c.name, "PASS")) {
            if (c.op == '=' && strlen(c.arg) < sizeof next.pass) strcpy(next.pass, c.arg);
            else err = true;            // never printed
        } else if (c.prefix == '$' && !strcmp(c.name, "PORT")) {
            long p = atol(c.arg);
            if (c.op == '?') info("$PORT: %u", next.port);
            else if (c.op == '=' && p >= 1 && p <= 65535) next.port = (uint16_t)p;
            else err = true;
        }
        // everything else parses and is ignored
    }
    if (err) { result("ERROR"); return; }
    result("OK");
    apply(next);
    if (save) {
        EEPROM.put(0, cfg);             // put() marks dirty only if bytes differ
        EEPROM.commit();
    }
}

// Command mode (IDLE / ONLINE_CMD): echo, line editing, CR runs the line.
static void commandByte(char ch) {
    if (ch == '\r') {
        if (cfg.echo) Modem.write('\r');
        line[nline] = 0;
        if (lineOverflow) result("ERROR");
        else runLine();
        nline = 0;
        lineOverflow = false;
    } else if (ch == 8 || ch == 127) {  // BS / DEL
        if (nline > 0) {
            nline--;
            if (cfg.echo) Modem.print("\b \b");
        }
    } else if (ch != '\n') {
        if (cfg.echo) Modem.write(ch);
        if (nline < (int)sizeof line - 1) line[nline++] = ch;
        else lineOverflow = true;
    }
}

// Data mode, host → TCP. A '+' after >= GUARD_MS of silence starts holding
// up to three; anything else releases them as data. checkGuard() decides.
static void pumpToTcp() {
    uint8_t in[128], out[sizeof in + 3];
    int room = client.availableForWrite() - 3;  // 3: held '+' that may go out too
    int n = min(Modem.available(), min(room, (int)sizeof in));
    if (n <= 0) return;
    n = Modem.readBytes(in, n);
    uint32_t t = millis();
    int m = 0;
    for (int i = 0; i < n; i++) {
        bool quiet = t - lastModemMs >= GUARD_MS;
        lastModemMs = t;
        if (in[i] == '+' && nheld < 3 && (nheld > 0 || quiet)) {
            nheld++;
            continue;
        }
        while (nheld > 0) { out[m++] = '+'; nheld--; }
        out[m++] = in[i];
    }
    if (m) client.write(out, m);
}

static void checkGuard() {
    if (nheld == 0 || millis() - lastModemMs < GUARD_MS) return;
    if (nheld == 3) {
        state = ONLINE_CMD;
        nline = 0;
        result("OK");
    } else {
        client.write((const uint8_t *)"++", nheld);  // lone '+' or '++': just data
    }
    nheld = 0;
}

static bool hostReady() {             // may we send to the host?
    return true;
}

// TCP → host, only what the UART can take now; the rest waits in lwIP and
// the TCP window closes (backpressure).
static void pumpToModem() {
    uint8_t buf[128];
    if (!hostReady()) return;
    int n = min(client.available(), min(Modem.availableForWrite(), (int)sizeof buf));
    if (n <= 0) return;
    n = client.read(buf, n);
    if (n > 0) Modem.write(buf, n);
}

// Drop a caller the host never heard about.
static void dropCaller() {
    client.stop();
    callerIp = 0;
    state = IDLE;
}

// Answer, refuse (banned: silently) or send BUSY.
static void acceptCaller() {
    WiFiClient c = server.accept();
    if (!c) return;
    uint32_t ip = c.remoteIP();
    if (banCheck(ip, millis())) { c.stop(); return; }
    if (state != IDLE) {
        c.print("BUSY, PLEASE TRY AGAIN LATER\r\n");
        c.stop();
        return;
    }
    client = c;
    client.setNoDelay(true);
    callerIp = ip;
    nearly = 0;
    answerMs = millis();
    state = ANSWERING;
}

// A new caller's first 4 bytes decide: "GET " is an HTTP scanner, banned
// and dropped. So is a caller that hangs up first. Anything else, or
// ANSWER_MS without 4 bytes, is answered and the bytes go to the host.
static void checkAnswer() {
    while (nearly < 4 && client.available()) early[nearly++] = (uint8_t)client.read();
    if (nearly == 4 && !memcmp(early, "GET ", 4)) {
        banStrike(callerIp, millis());
        dropCaller();
        return;
    }
    if (!client.connected() && !client.available()) { dropCaller(); return; }
    if (nearly < 4 && millis() - answerMs < ANSWER_MS) return;
    nline = 0;
    goOnline();
    Modem.write(early, nearly);
}

// FLASH held BAUD_RESET_MS → 19200 in RAM (AT&W keeps it). Once per press.
static void checkBaudReset() {
#ifdef PIN_BAUD_RESET
    static uint32_t downSince = 0;
    static bool done = false;
    if (digitalRead(PIN_BAUD_RESET) != LOW) { downSince = 0; done = false; return; }
    if (downSince == 0) downSince = millis() | 1;
    if (!done && millis() - downSince >= BAUD_RESET_MS) {
        Config n = cfg;
        n.baud = 19200;
        apply(n);
        done = true;
    }
#endif
}

void setup() {
#ifdef PIN_CTS_OUT
    digitalWrite(PIN_CTS_OUT, PIN_ASSERTED);
    pinMode(PIN_CTS_OUT, OUTPUT);
#endif
#ifdef PIN_RTS_IN
    pinMode(PIN_RTS_IN, INPUT);
#endif
#ifdef PIN_BAUD_RESET
    pinMode(PIN_BAUD_RESET, INPUT_PULLUP);
#endif
    EEPROM.begin(sizeof(Config));
    loadProfile(cfg);
    modemBegin(cfg.baud);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    wifiBegin();
    server.begin(cfg.port);
}

void loop() {
    checkBaudReset();
    acceptCaller();
    if (state == ANSWERING) checkAnswer();
    if (state == ONLINE) {
        pumpToModem();
        pumpToTcp();
        checkGuard();
    } else {
        while (Modem.available() && state != ONLINE) commandByte((char)Modem.read());  // IDLE, ANSWERING, ONLINE_CMD
        if (state == ONLINE_CMD) {      // remote data in command mode is dropped, as in modem.c
            uint8_t buf[128];
            while (client.available()) client.read(buf, sizeof buf);
        }
    }
    if ((state == ONLINE || state == ONLINE_CMD) && !client.connected() && !client.available()) hangup();
}
```

- [ ] **Step 2: Build**

Run: `pio run`
Expected: `[SUCCESS]`, no warnings from `src/` or `include/`.

- [ ] **Step 3: Flash and configure WiFi** (hardware-in-the-loop)

Run: `pio run -t upload`. Then in a terminal on the DB9 at 19200 (e.g. `picocom -b 19200 /dev/ttyUSB1`), type:

```
AT$SSID=<your ssid>
AT$PASS=<your password>
AT&W
ATI
```

Expected: `OK` after each, and `ATI` shows `WIFI: CONNECTED <ssid> <ip>` within a few seconds (repeat `ATI` if it says `DISCONNECTED`). Note the IP.

- [ ] **Step 4: Run the hardware tests** (hardware-in-the-loop)

Run: `tools/hwtest.py --port /dev/ttyUSB1 --modem <modem ip> --me <this machine's ip>`
Expected: `config ok`, then `calls ok`. Takes about a minute (`calls` includes 12 s dial timeouts).

- [ ] **Step 5: Check the FLASH-button baud reset by hand** (hardware-in-the-loop)

In the terminal: `AT+IPR=9600` → `OK`, then the terminal shows garbage at 19200. Hold FLASH for 3 s (after boot, never while pressing RST). Type `AT` at 19200.
Expected: `OK`. Then `ATZ` → `OK` (flash still holds 19200).

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "Add modem firmware: AT commands, profile, calls and IP bans"
```

---

### Task 6: RTS/CTS flow control (`&K3`)

**Files:**
- Modify: `tools/hwtest.py` (add `test_flow`, add `flow` to the default sections)
- Modify: `src/main.cpp` (`hostReady()`, new `updateCts()`, one line in `loop()`)

**Interfaces:**
- Consumes: `hostReady()`, `cfg.flow`, `loop()` from Task 5; `PIN_RTS_IN`, `PIN_CTS_OUT`, `PIN_ASSERTED`, `MODEM_RX_BUFFER` from Task 1; hwtest helpers from Task 4.

- [ ] **Step 1: Write the failing test**

In `tools/hwtest.py`, insert this function directly above `def main():`:

```python
def test_flow(ser, a):
    at(ser, b"ATZ")
    at(ser, b"ATE0")
    eq(at(ser, b"AT&K3"), OK)
    peer_l = listener(a.me, rcvbuf=4096)                # small window: the peer fills fast
    ser.write(f"ATDT{a.me}:{PEER_PORT}\r".encode())
    peer, _ = peer_l.accept()
    eq(rd(ser, 12), CONNECT)
    # RTS: while the host holds it off, TCP data waits
    ser.rts = False
    peer.sendall(b"abc")
    eq(rd(ser, 1), b"")
    ser.rts = True
    eq(rd(ser), b"abc")
    # CTS: the peer stops reading; the modem drops CTS before its buffer
    # overflows, and nothing is lost once the peer reads again
    sent = bytearray()
    end = time.time() + 30
    while ser.cts and time.time() < end:
        sent += bytes((len(sent) + i) % 251 for i in range(64))
        ser.write(sent[-64:])
        ser.flush()
    assert not ser.cts, "CTS never dropped"
    got = bytearray()
    peer.settimeout(3)
    try:
        while len(got) < len(sent):
            more = peer.recv(65536)
            if not more:
                break
            got += more
    except socket.timeout:
        pass
    assert got == sent, f"peer got {len(got)} bytes, host sent {len(sent)}"
    time.sleep(0.5)
    assert ser.cts, "CTS did not come back"
    escape(ser)
    eq(at(ser, b"ATH"), NOCARRIER)
    peer.close()
    # &K0: RTS is ignored
    eq(at(ser, b"AT&K0"), OK)
    ser.write(f"ATDT{a.me}:{PEER_PORT}\r".encode())
    peer, _ = peer_l.accept()
    eq(rd(ser, 12), CONNECT)
    ser.rts = False
    peer.sendall(b"xyz")
    eq(rd(ser), b"xyz")
    ser.rts = True
    escape(ser)
    eq(at(ser, b"ATH"), NOCARRIER)
    peer.close()
    peer_l.close()
    eq(at(ser, b"ATZ"), OK)
```

Then change the default sections and the usage line:

```python
    ap.add_argument("sections", nargs="*", default=["config", "calls", "flow"])
```

```
  tools/hwtest.py --port /dev/ttyUSB0 --modem 192.168.1.50 --me 192.168.1.10 [config] [calls] [flow]
```

- [ ] **Step 2: Run it to see it fail** (hardware-in-the-loop)

Run: `tools/hwtest.py --port /dev/ttyUSB1 --modem <modem ip> --me <this machine's ip> flow`
Expected: `AssertionError: got b'abc', want b''` (Task 5 ignores RTS).

- [ ] **Step 3: Honour RTS**

In `src/main.cpp`, replace:

```cpp
static bool hostReady() {             // may we send to the host?
    return true;
}
```

with:

```cpp
static bool hostReady() {             // may we send to the host?
#ifdef PIN_RTS_IN
    if (cfg.flow == 3 && digitalRead(PIN_RTS_IN) != PIN_ASSERTED) return false;
#endif
    return true;
}
```

- [ ] **Step 4: Drive CTS**

Insert above the `// Answer, refuse (banned: silently) or send BUSY.` comment:

```cpp
// CTS under &K3: drop at ¾ full, raise again at ¼; else always asserted.
static void updateCts() {
#ifdef PIN_CTS_OUT
    static bool asserted = true;
    int used = Modem.available();
    if (cfg.flow != 3) asserted = true;
    else if (used >= MODEM_RX_BUFFER * 3 / 4) asserted = false;
    else if (used <= MODEM_RX_BUFFER / 4) asserted = true;
    digitalWrite(PIN_CTS_OUT, asserted ? PIN_ASSERTED : !PIN_ASSERTED);
#endif
}
```

In `loop()`, add the call as the last line:

```cpp
    if ((state == ONLINE || state == ONLINE_CMD) && !client.connected() && !client.available()) hangup();
    updateCts();
}
```

- [ ] **Step 5: Build, flash and run all hardware tests** (hardware-in-the-loop)

Run: `pio run -t upload`, then `tools/hwtest.py --port /dev/ttyUSB1 --modem <modem ip> --me <this machine's ip>`
Expected: `config ok`, `calls ok`, `flow ok`.

- [ ] **Step 6: Run the native tests once more**

Run: `pio test -e native`
Expected: `16 test cases: 16 succeeded`.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp tools/hwtest.py
git commit -m "Add RTS/CTS flow control (AT&K3)"
```
