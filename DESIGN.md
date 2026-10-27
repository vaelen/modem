# Modem firmware: design

A Hayes-style WiFi modem for the TheOldNet v6 board (ESP8266 NodeMCU, see `HARDWARE.md`). It answers 
TCP callers, dials out to `host[:port]`, handles the `+++` escape, and bans abusive callers by IP 
with `AT+BAN`. Everything else is left out.

## Goals and non-goals

- **In:**
  - Inbound and outbound TCP calls.
  - Hayes command mode.
  - IP bans.
  - Configuration through AT commands on the DB9, stored in flash.
  - Optional RTS/CTS.
  - A status LED.
- **Out:** SLIP, PPP, telnet IAC handling, web UI, mDNS, OTA, SD card, PETSCII, speed dial,
  soft-AP setup mode, and a USB console or log. The ESP8266's only full-duplex UART drives the
  DB9, so a USB console would need SoftwareSerial. It isn't worth it.
- **Defaults:** the DB9 runs at **19200 8N1**.

## Files

```
platformio.ini                env:nodemcuv2 (firmware), env:native (unit tests)
include/board.h               everything board-specific: Modem alias, pins, WiFi header, modemBegin()
include/at.h, src/at.cpp      AT command-line tokenizer; plain C++, no Arduino calls
include/bans.h, src/bans.cpp  strike table; plain C++, no Arduino calls (time passed in)
src/main.cpp                  setup/loop, call state machine, command handlers, config load/save
test/test_at/                 Unity test for the tokenizer (native)
test/test_bans/               Unity test for ban durations, escalation, expiry, table-full reuse (native)
tools/hwtest.py               end-to-end test against real hardware (port of test.sh; pyserial + sockets)
```

`main.cpp` includes only `board.h`, `at.h`, `bans.h`, `<EEPROM.h>` and the Arduino core. It never names `Serial`,
`Serial1` or a GPIO number.

## Board layer (`include/board.h`)

The board layer is a single header with one `#if` block per board:

```cpp
#if defined(ARDUINO_ESP8266_NODEMCU_ESP12E)       // TheOldNet v6 carrier
  #include <ESP8266WiFi.h>
  #define Modem   Serial        // UART0, swapped to GPIO13/15 → DB9
  #define MODEM_RX_BUFFER  1024 // UART receive buffer; the CTS thresholds are ¾ and ¼ of it
  #define PIN_CTS_OUT      12   // D6 → DB9 pin 8
  #define PIN_RTS_IN       14   // D5 ← DB9 pin 7
  #define PIN_ASSERTED     LOW  // MAX3232 inverts: TTL LOW = RS-232 +V
  #define PIN_BAUD_RESET   0    // FLASH button, active LOW
  #define PIN_LED          16   // D0: the NodeMCU's LED
  #define PIN_LED_ON       LOW
  inline void modemBegin(uint32_t baud) {
    Serial.setRxBufferSize(MODEM_RX_BUFFER); Serial.begin(baud); Serial.swap();
  }
  inline void modemBaud(uint32_t baud) { Serial.updateBaudRate(baud); }  // AT+IPR; begin() glitches TX
#else
  #error "Add your board to board.h"
#endif
```

Porting to a new board means adding one block to this header. For example, an ESP32 block would use
`#define Modem Serial2` and `Serial2.begin(baud, SERIAL_8N1, RX, TX)`. A board without a given line
(RTS/CTS, the reset button) leaves that pin undefined, and the code that uses it compiles out under
`#ifdef`.

Before the swap, the boot ROM prints to USB at 74880 baud, so the DB9 host never sees that output.
After the swap, the USB port is silent.

## Network

- **Station mode only:**
  - `WiFi.persistent(false)`, because we store the credentials ourselves.
  - `WiFi.setAutoReconnect(true)`, then `WiFi.begin(ssid, pass)` at boot and whenever `AT$SSID` or
    `AT$PASS` changes.
  - DHCP only.
- **Listener:** a `WiFiServer` on the configured port (default 23). It restarts when `AT$PORT` changes.
- **Inbound:** `server.accept()` each loop, checked in this order:
  1. Banned IP: `stop()` without a word, and the host never sees it (same as `modem.c`).
     So is a caller redialing within 10 s of its last call (`REDIAL_MS`) for the 3rd time running
     (`REDIAL_MAX`). That's a strike too, and the count starts over. Only the last caller's IP is
     tracked.
  2. A call is already up: send `BUSY, PLEASE TRY AGAIN LATER\r\n`, then `stop()`.
  3. Otherwise the caller is screened (`SCREENING`), and its IP is kept for `AT+BAN`. The host
     hasn't been told yet. The modem waits up to 1 s (`ANSWER_MS`) for the caller's first 4 bytes:
     - **`GET `:** an HTTP scanner. It's banned (one strike, as `AT+BAN`) and dropped. The host never
       sees it.
     - **The caller hangs up first:** dropped. The host never sees it.
     - **Anything else, or 1 s passes:** it rings (`RINGING`): `RING` now and every 6 s (`RING_MS`).
       On the S0th ring, or on `ATA`, it's answered: `CONNECT <baud>`, then the held bytes go to
       the host. A caller that hangs up while ringing is dropped. Under `S0=0`, one nobody answers
       is dropped after 10 rings (`RING_MAX`), so a silent bot can't hold the line forever.

     The wait costs a caller who says nothing first up to 1 s. Scanners send their request at once.

  There's no "serial port down" case on hardware, because DTR isn't wired.
- **Outbound:**
  - `ATD[T|P]host[:port]`, with spaces stripped and port 23 by default. Dashes are kept, unlike
    `modem.c`, because there's no address book here and hostnames can contain them. A string without
    `.` or `:` gives `NO CARRIER`.
  - `client.connect()` is **blocking**, with a 10 s timeout that covers DNS too.
  - `ponytail:` a keypress can't abort a dial while it's in progress, unlike `modem.c`. If that matters,
    switch to an async connect: lwIP raw API, or `AsyncTCP` on ESP32.
- **Every call:** `setNoDelay(true)`.

## Call state machine (`main.cpp`)

`modem.c` has four states. `DIALING` goes away because the dial is blocking, and `SCREENING` and
`RINGING` are new (see Inbound), which gives `IDLE`, `SCREENING`, `RINGING`, `ONLINE` and `ONLINE_CMD`.
While `SCREENING` or `RINGING` the host is still in command mode, but `ATD` gives `ERROR` (the line is
busy).

Each `loop()` pass does this:

1. Baud reset: if FLASH has been held for 3 s, do `AT+IPR=19200` (RAM only, like any setting).
2. `accept()` a new caller, check a `SCREENING` caller's first bytes, and ring a `RINGING` one (see
   Network).
3. `ONLINE`:
   - **Modem → TCP:** bytes go through the `+++` detector (the same held-`+` logic as `modem.c`, with a
     500 ms guard) into a small buffer that is written to the client. The call's timeout is 0, so
     `write()` never blocks `loop()`. lwIP can refuse bytes (its segment queue can fill before its send
     buffer does), and refused bytes stay in the buffer for the next pass. While any are waiting, host
     bytes aren't read, so they back up in the UART receive buffer and CTS (step 5) stops the host
     before it overflows. (The core's default is to block for the whole timeout, then drop the rest.)
   - **TCP → Modem:** copy only `min(client.available(), Modem.availableForWrite())` bytes, and nothing
     at all while RTS is deasserted under `&K3`. Unread data stays in lwIP and the TCP window closes.
     That's the backpressure `LQ_MAX` gave `modem.c`, with no queue to manage.
   - `!client.connected() && !client.available()`: send `NO CARRIER` and go to `IDLE`.
4. `IDLE` / `SCREENING` / `RINGING` / `ONLINE_CMD`:
   - Modem bytes build the command line. Echo is on under `E1`, and BS/DEL erase a character.
   - Bytes before the first `A` are skipped, and the line is parsed from its first `AT`, as on a Hayes
     modem. Line noise (a baud mismatch, the boot glitch) can't swallow the next command.
   - CR runs the parser.
   - TCP data is dropped in `ONLINE_CMD`, as in `modem.c`.
5. CTS, under `&K3`: deassert when `Modem.available()` reaches ¾ of `MODEM_RX_BUFFER`, and assert again
   once it drops to ¼. The gap between the two keeps the line from flapping. The remaining ¼ of the buffer
   (256 bytes) absorbs whatever the host's UART has already queued when CTS drops; a 16550 FIFO is 16
   bytes. Under `&K0`, CTS stays asserted.
6. Guard check: after 500 ms of silence, held `+++` becomes `ONLINE_CMD` + `OK`, and a shorter run of
   held `+` is sent on as data.

All result codes are verbose: `\r\n<text>\r\n`. There are no numeric codes.

## AT commands

The tokenizer (`at.cpp`) is the one from `modem.c`, split out so it can be unit-tested, with two changes:

- **Only command letters are uppercased.** The values of `$SSID`/`$PASS` keep their case.
- **`$SSID=`, `$PASS=` and `D` consume the rest of the line verbatim.** This allows `;` and spaces in
  passwords.

Anything unrecognised still parses cleanly and returns `OK`, so terminal init strings work.

| Command         | Effect                                                             | Profile |
|-----------------|--------------------------------------------------------------------|---------|
| `ATD[T\|P]h[:p]`| dial; `CONNECT <baud>` / `NO CARRIER`; `ERROR` if not on-hook      |         |
| `ATH[0]`        | online-cmd: hang up; `OK` either way, as on a Hayes                |         |
| `ATO`           | online-cmd: back to data, `CONNECT <baud>`                         |         |
| `ATA`           | answer a ringing caller; `NO CARRIER` if none, `ERROR` online-cmd  |         |
| `ATS0=n`        | answer on the nth `RING` (0–254, default 1); 0 = only `ATA`        | yes     |
| `AT+BAN`        | strike the current inbound caller; `ERROR` if none                 |         |
| `AT+BAN=0`      | clear the ban list                                                 |         |
| `AT+BAN?`       | list the ban table, one line per IP (see below)                    |         |
| `ATE0` / `ATE1` | command echo off/on (default 1)                                    | yes     |
| `AT&K0`/`AT&K3` | flow control none / RTS-CTS (default 0)                            | yes     |
| `AT+IPR=n`      | DB9 baud (300…115200); `OK` sent at the old rate, then switch      | yes     |
| `AT$SSID=text`  | WiFi SSID; reconnects                                              | yes     |
| `AT$PASS=text`  | WiFi password; reconnects; never printed                           | yes     |
| `AT$PORT=n`     | listen port (default 23); restarts the listener                    | yes     |
| `AT...?`        | `&K?`, `+IPR?`, `$SSID?`, `$PORT?` print the value; `S0?` as `001` |         |
| `ATI`           | status: version, WiFi state/SSID/IP or why not, port, baud, flow,  |         |
|                 | S0, bans                                                           |         |
| `AT&W`          | write the current profile to flash                                 |         |
| `AT&F`          | factory defaults into RAM, keeping WiFi and port; flash untouched  |         |
| `ATZ`           | hang up if online, then reload the profile from flash and apply it |         |

- **Profile:** SSID, password, baud, port, `E`, `&K` and `S0`. Every setting takes effect as soon as it's
  set but lives only in RAM until `AT&W`. A bad change is undone with `ATZ` or a reboot.
- **`AT&F` keeps the SSID, password and listen port.** Terminal programs often send `AT&F` in their init
  strings, and that mustn't knock the modem off the network. The full factory reset is `AT$SSID=`,
  `AT$PASS=`, `AT&F`, `AT&W`.
- **Applying a profile (`ATZ`, `AT&F`)** only touches what changed. WiFi reconnects only if the SSID or
  password differ, the listener restarts only if the port differs, and the UART re-inits only if the baud
  differs. Many terminal programs send `ATZ` on startup, and that shouldn't drop WiFi.
- **`AT+BAN?` output:** one line per entry, then `OK`. Expired entries are listed too, because their
  strike count still drives the next ban's length (until it's a day old).

  ```
  +BAN: 203.0.113.7,2,41       IP, strikes, minutes left (0 = not blocked now)
  +BAN: 198.51.100.23,1,0
  OK
  ```
- **Baud recovery:** if you set a rate your terminal can't use, hold FLASH for 3 s once the board has
  booted, and the DB9 goes back to 19200 in RAM. Send `AT&W` to keep it. Don't hold it during reset:
  GPIO0 held LOW at reset puts the ESP8266 into its bootloader.

## Config storage

Arduino `EEPROM` (on ESP it's flash-backed: `EEPROM.begin(sizeof cfg)` / `commit()`). The profile in RAM
and the one in flash are the same struct:

```cpp
struct Config {
  uint32_t magic;     // 'MDM1'; mismatch → defaults (first boot, layout change)
  char ssid[33];
  char pass[65];
  uint32_t baud;      // 19200
  uint16_t port;      // 23
  uint8_t echo;       // 1
  uint8_t flow;       // 0 = none, 3 = RTS/CTS
  uint8_t s0;         // 1; auto-answer ring, 0 = never
};
```

`s0` was added after the layout shipped. Flash past the old struct is erased (0xFF), so `0xFF` loads as
the default 1 and saved profiles keep their WiFi. That's why `S0` stops at 254.

Flash is written only by `AT&W`, and only when the bytes differ from what's already stored.

## IP bans (`bans.cpp`)

These are the same rules as `modem.c`:

- Strike 1 blocks for 5 min, strike 2 for 1 h, strike 3 and up for 24 h.
- If the last strike is more than 24 h old, the count starts over.
- The list lives in RAM and is lost on reboot.
- `banRedial(ip, now)` is called for every unbanned inbound call. It strikes an IP that redials too
  fast (see Inbound) and returns true.

API: `banStrike(ip, now)`, `banCheck(ip, now) → ms left or 0`, `banClear()`, `banCount()`, and
`banEntry(i, &ip, &strikes, &msLeft, now)` for `AT+BAN?`. IPs are
`uint32_t` (`(uint32_t)client.remoteIP()`), and times are `millis()`.

- `ponytail:` **256 entries** (3 KB), not 8192. RAM on the ESP8266 is about 40 KB. When the table is
  full, the stalest entry is reused, same as `modem.c`.
- `ponytail:` `millis()` wraps at 49 days. An entry that hasn't been touched for more than 49 days could
  briefly look fresh again. The fix, if it ever matters, is to prune entries older than 24 h on each
  strike.

## Hardware lines (v6)

- **CTS out** (GPIO12): asserted at boot. Under `&K3` it follows how full the UART receive buffer is (see
  step 5 above). This normally only matters when the far end stops reading and the TCP send buffer fills.
- **RTS in** (GPIO14): read only under `&K3`. While it's deasserted, TCP → Modem stops (see step 3 above).
- `&K3` changes both lines together. There's no separate setting for one direction.
- **Not wired on the board:** DCD, DSR, DTR and RI. Hosts that wait for DCD/DSR need a cable that loops
  DTR → DCD+DSR. Hanging up on a DTR drop isn't possible on this board.

## Status LED

The NodeMCU's own LED (GPIO16, active LOW). The ESP-12 module's blue LED on GPIO2 flickers during boot
and flashing, so it isn't used.

| LED              | Meaning                                                      |
|------------------|--------------------------------------------------------------|
| solid on         | WiFi connected                                               |
| 1 blink/s        | trying to connect                                            |
| 4 blinks/s       | WiFi error: network not found, wrong password, failed, lost  |
| 10 blinks in 1 s | FLASH-button baud reset, then back to one of the above       |
| off              | no WiFi configured                                           |

"Error" is whatever `ATI` shows as the reason in `WIFI: DISCONNECTED <ssid> (<reason>)`. The ESP8266
keeps retrying after an error, so the LED can alternate between 4/s and 1/s. `ponytail:` the LED
freezes while a dial blocks (up to 10 s).

## Testing

- **Native Unity tests:** `pio test -e native` runs `test/test_at` and `test/test_bans`. They cover the
  tokenizer (case kept in values, rest-of-line arguments, chained and junk input) and the ban rules
  (durations, escalation, the 24 h reset, `millis()` wrap and reuse of the stalest entry).
- **`tools/hwtest.py`:** `test.sh` rewritten to drive the DB9 through a USB-RS232 adapter (pyserial) and
  call in over WiFi. It covers:
  - inbound CONNECT and data both ways
  - `+++` with and without guard time
  - ATO and ATH
  - caller hangup
  - dial out, and ERROR while online
  - BUSY
  - flood versus escape
  - `AT+BAN;H` followed by silent refusal
  - `&K3`: a TCP peer that stops reading makes CTS drop (pyserial `cts`), with no bytes lost once it
    reads again. A host that drops RTS pauses TCP → DB9.
  - `AT+BAN?` listing, then `AT+BAN=0`
  - `AT&W` / `AT&F` / `ATZ` round trip (settings survive a reboot only after `AT&W`)
  - the chained-command parse, and line noise before `AT`
  - the `GET ` scanner ban, and callers that hang up before they're answered
  - `RING` and `ATA` under `S0=0`, a caller hanging up mid-ring, and `S0=2`

  The exec and dial.conf cases are dropped. `--no-cts` skips the CTS check for adapters that don't
  report CTS (common with CH340 cables). The FLASH-button reset is checked by hand.

## Build order

1. `board.h` + a stub `main.cpp` that echoes on the DB9. **This proves the swap and the pin map on real v6
   hardware.**
2. Config + AT parser (the config commands and `ATI`), with WiFi join.
3. Call state machine: inbound, outbound, `+++`.
4. Bans + native test.
5. `&K3` + `hwtest.py`.

## Skipped (add when needed)

| Skipped                   | Add when                                                        |
|---------------------------|-----------------------------------------------------------------|
| Speed dial / address book | a host dialer can only send digits                              |
| Async, abortable dial     | the blocking 10 s dial proves annoying                          |
| Persistent bans           | reboots are frequent enough that bots get a fresh start         |
| Soft-AP setup mode        | there's no host terminal for first-time WiFi setup              |
| Debug log on Serial1      | debugging needs it: TX-only on GPIO2 (free on v6), not on USB   |
