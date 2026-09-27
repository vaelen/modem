# TheOldNet Serial WiFi Modem: Hardware Reference

This document is based on reading `firmware/theoldnet_serial_wifi_modem/theoldnet_serial_wifi_modem.ino`
(build `03202022-ppp`), the README, the docs in `docs/`, and the product photo `docs/modemv3.jpg`.
Anything marked **(unverified)** comes from the photo or from typical parts. Check it with a
multimeter before you rely on it.

## 1. Hardware revisions

All known revisions are **ESP8266** (NodeMCU dev board). The firmware in this repo is the v3-era
firmware. The v4 and v6 firmware was only ever distributed as binaries in the flash-tool zip.

- **v3:** NodeMCU plus a MAX3232 DB9 daughterboard (§2c–§3). The pin map in §3 comes from the firmware source.
- **v6 ("Serial Wifi Adapter v6.0"):** NodeMCU soldered onto a white carrier PCB with the DB9, the
  level shifter and a micro-SD reader. The silkscreen says "Hayes Modem, PPP & SLIP, Ethernet
  Compatible". **The pinout is different from v3.** See §2a.

**Before flashing your own firmware, back up the stock image.** You can't get it back any other way:

```
esptool.py --port /dev/ttyUSB0 read_flash 0 ALL v6-stock.bin
```

Running `strings` on the dump is also the cheapest way to find the v6 feature set, and maybe some pin hints.

## 2a. v6 carrier board

The dev board is a NodeMCU v2-style board: ESP-12E/F "ESP8266MOD", CP2102 USB-UART, AMS1117 LDO,
and FLASH/RST buttons. It is soldered directly to the carrier through both 15-pin rows.

**v6 pinout: taken to match the silkscreen.** The DB9 side was checked with a meter, and the
serial swap was confirmed from the stock firmware.

| Signal (DB9 pin)    | ESP side                                                        |
| ------------------- | --------------------------------------------------------------- |
| TX out (2)          | GPIO15 / D8 (swapped UART TX)                                   |
| RX in (3)           | GPIO13 / D7 (swapped UART RX)                                   |
| RTS in (7)          | GPIO14 / D5                                                     |
| CTS out (8)         | GPIO12 / D6                                                     |
| DCD, DSR, DTR, RI   | not connected                                                   |
| USB serial (CP2102) | GPIO1 / GPIO3 (unswapped)                                       |
| micro-SD            | unknown, among D0–D4 (GPIO16/5/4/0/2); unused by stock firmware |

Level shifter: a 16-pin MAX3232-class chip. The rest of this section is how that was worked out.

The back of the carrier labels 8 pins at the USB end of the **D-pin row**. Flipping the photo over,
using the square pin-1 pad, the "HOLD FLASH →" arrow and the 3V at the end of the row, lines those
labels up with these dev-board pins:

| Carrier label | NodeMCU | ESP8266 GPIO | Notes                                                              |
|---------------|---------|--------------|--------------------------------------------------------------------|
| RTS           | D5      | GPIO14       | Plain GPIO (no UART function here). Direction/naming unconfirmed   |
| CTS           | D6      | GPIO12       | Plain GPIO. Direction/naming unconfirmed                           |
| RX1           | D7      | GPIO13       | UART0 RX **after `Serial.swap()`**                                 |
| TX1           | D8      | GPIO15       | UART0 TX after `Serial.swap()`. Boot strap: must be LOW at reset   |
| RX0           | RX      | GPIO3        | UART0 RX default. Shared with the CP2102                           |
| TX0           | TX      | GPIO1        | UART0 TX default. Shared with the CP2102                           |
| GND           | GND     |              |                                                                    |
| 3V            | 3V3     |              | Level shifter is probably powered from 3.3V                        |

What this suggests (needs a continuity check):

- **Confirmed by the stock firmware dump (§2b): the DB9 is on GPIO13/15 via `Serial.swap()`.**
  The firmware boots on UART0's default pins (USB/CP2102) and prints the banner. It then asks
  "Stay in USB Mode? Press ANY key quickly." If no key is pressed, it swaps to "Serial 1 (DE9)".
  `AT$SWAP` toggles between the two. The firmware also checks for a "compatible PCB" before
  swapping ("SERIAL SWAP PCB = …"). How it detects this isn't visible in the strings.
- **The flow-control lines moved to GPIO14/12**, which frees GPIO2 and GPIO16.
- **The SD reader can't be on hardware SPI (HSPI).** HSPI uses GPIO12–15, which are all taken by
  the pins above. The only pins left are D0 (GPIO16), D1 (GPIO5), D2 (GPIO4), D3 (GPIO0), D4 (GPIO2),
  and SD3/SD2 (GPIO10/9, which are flash pins; only GPIO10 is sometimes usable, and only in DIO flash mode).
  So SD probably uses bit-banged SPI on GPIO4/5/… and GPIO0 or GPIO2 as CS. The other possibility
  is that the SD socket was wired to the NodeMCU "SD0–SD3/CMD/CLK" pins. **Those are the ESP-12's
  internal flash bus, not an SD card interface.** If that's how it's wired, the SD card can't be used.

### v6 DB9 solder pads (back of carrier)

All 9 DB9 pins have labelled pads. The carrier's white solder mask hides the traces, so
**having a pad doesn't mean the pin is routed anywhere.**

| DB9 pin | Carrier label | Standard RS-232 name | Direction (modem = DCE) |
|---------|---------------|----------------------|-------------------------|
| 1       | CD            | DCD                  | modem → host            |
| 2       | TX            | RXD                  | modem → host            |
| 3       | RX            | TXD                  | host → modem            |
| 4       | DTR           | DTR                  | host → modem            |
| 5       | GND           | GND                  |                         |
| 6       | DSR           | DSR                  | modem → host            |
| 7       | RTS           | RTS                  | host → modem            |
| 8       | CTS           | CTS                  | modem → host            |
| 9       | RI            | RI                   | modem → host            |

- **TX/RX are labelled from the modem's side:** pin 2 is the modem's TX. **RTS/CTS use the
  standard DB9 names from the computer's side**, so "RTS" (pin 7) is an input to the modem. Expect
  the same mixed naming on the dev-board header ("RTS" = D5/GPIO14, "CTS" = D6/GPIO12).
- **The level shifter is a 16-pin MAX3232-class chip** (see below). It has 2 drivers and 2
  receivers, which matches the 4 header signals: TX1, RX1, RTS and CTS. So DCD, DSR, DTR and RI
  aren't driven by the ESP. They're either not connected or looped back.

**Photo of traces on the back of the carrier:**
- Traces visibly leave the **CD, TX, RX and DTR** pads and run to a row of about 6–7 vias toward
  the middle of the board. They then continue under the silkscreen text toward the dev board.
- No trace is visible on this layer from **DSR, RTS, CTS or RI**. They may be routed on the top
  layer, which is hidden behind the DB9 shell. **RTS/CTS are wired:** the stock v6 firmware supports hardware flow control.
- The level shifter isn't visible. The via row probably goes to a single IC on the top side, under the dev board, which is soldered on and not socketed.
- The CD/DTR traces turned out not to carry driven signals (see the measurements below).

**Later photos:**
- The CD, TX, RX and DTR traces go to a single column of about 8 vias. That fits one side of an
  SOIC/SSOP IC on the top of the carrier.
- A side view between the boards shows a gull-wing SOIC-class IC on the carrier, under the dev
  board. Its marking isn't readable.
- A bundle of 3–4 traces runs diagonally from the via area to the labelled header end
  (RTS…3V). One trace runs around the board edge, probably 3V or GND.
- **Close-ups show a 16-pin SOIC** (8 gull-wing leads per side) with small SMD capacitors next
  to it. Those are the charge-pump caps. **This is a MAX3232-class transceiver** (MAX3232, SP3232,
  ST3232, HIN232…), so there are **2 drivers and 2 receivers**. Those four channels match the
  header exactly: TX and CTS out, RX and RTS in.
- **Measured with power on, stock firmware idle, nothing plugged into the DB9, against pin 5:**

  | DB9 pin | Signal | Reading     | Meaning                                     |
  | ------- | ------ | ----------- | ------------------------------------------- |
  | 2       | TX     | about -5V   | Driver, idle mark: MAX3232 output           |
  | 8       | CTS    | about +5V   | Driver, asserted: MAX3232 output            |
  | 3       | RX     | about 0V    | Expected MAX3232 receiver input (5k to GND) |
  | 7       | RTS    | about 0V    | Expected MAX3232 receiver input (5k to GND) |
  | 4       | DTR    | about 0V    | Probably not connected (no receiver left)   |
  | 1       | CD     | ~0V, drifts | Floating: **not driven**                    |
  | 6       | DSR    | about 0V    | Not driven                                  |
  | 9       | RI     | about 0V    | Not driven                                  |

  **Only TX, CTS, RX and RTS are live.** DCD, DSR, DTR and RI aren't usable from firmware, and
  the CD/DTR traces on the back don't carry a driven signal. Hosts that wait for DCD or DSR need
  a cable/adapter that loops them (for example DTR→DCD+DSR on the host side).

  To confirm the inputs, measure resistance to pin 5 with the power off. Pins 3 and 7 should read
  about 5kΩ (the receiver's input resistor), and unconnected pins should read open.

MAX3232 pinout, for probing the leads visible from the side:

| Pin | Name   | Side    | Pin | Name  | Side    |
|-----|--------|---------|-----|-------|---------|
| 1   | C1+    | pump    | 9   | R2OUT | 3.3V    |
| 2   | V+     | pump    | 10  | T2IN  | 3.3V    |
| 3   | C1-    | pump    | 11  | T1IN  | 3.3V    |
| 4   | C2+    | pump    | 12  | R1OUT | 3.3V    |
| 5   | C2-    | pump    | 13  | R1IN  | RS-232  |
| 6   | V-     | pump    | 14  | T1OUT | RS-232  |
| 7   | T2OUT  | RS-232  | 15  | GND   |         |
| 8   | R2IN   | RS-232  | 16  | VCC   |         |


### v6 micro-SD socket

The side photo shows the micro-SD socket on the carrier, sandwiched under the dev board and opening
at the USB end, just below the NodeMCU's micro-USB. Its pins are hidden, so the wiring has to be
buzzed out.

**To confirm with a meter (power off):**

1. DB9 pins 1, 4, 6, 8, 9: does each go to the level shifter or nowhere? Is any tied to another DB9 pin or to GND/3V3?
2. The level-shifter chip part number, if you can see it between the boards.
3. D5/D6/D7/D8/RX/TX: which level-shifter pins do they reach? This tells you whether RX0/TX0 or RX1/TX1 carry the DB9 data.
4. Micro-SD pins 1 (DAT3/CS), 2 (CMD/MOSI), 5 (CLK), 7 (DAT0/MISO): which dev-board pins?

## 2b. v6 stock firmware (from `v6-stock.bin` strings)

- **Build `20230905`.** Same code lineage as this repo (same banner, AT help text and EEPROM
  defaults), plus the additions below.
- **Serial:** USB/DB9 swap as described above. `AT$SWAP` switches ports.
- **Storage: SPIFFS on internal flash.** The dump has no SD/SdFat/SDFS strings, so **the stock
  firmware doesn't appear to use the micro-SD reader at all.** SD pin assignments therefore
  can't be recovered from it.
- **File commands:**
  - `AT$LS`: list files.
  - `AT$UL`: YMODEM upload.
  - `AT$DL=/FILE`: YMODEM download.
  - `AT$CFS`: check the filesystem.
  - `AT$FMT`: prints "Not Implemented".
- **New AT commands:**
  - `ATGPH<url>`: Gopher request.
  - `AT$FW`: OTA update from `http://theoldnet.com/ota/`. This is plain HTTP, and the firmware
    checks `latest-version.txt` at boot.
- **Web UI:** a full status and settings page, a speed-dial editor, a file manager with
  upload/delete, `/dial`, `/reboot` and `/update-firmware`.
- **mDNS name:** `theoldnet`.
- **PPP** (`ATDTPPP`) as in the v3 source. **No SLIP or Ethernet-related strings were found**,
  despite what the silkscreen says.
- **The dump includes the EEPROM sector, which holds your WiFi SSID/password.** Don't share or
  commit `v6-stock.bin`.

## 2c. v3 board architecture (per the photo)

```
 micro-USB 5V ──► NodeMCU ESP8266 dev board (ESP-12E/F + CP2102 USB-UART + AMS1117 3.3V LDO)
                    │  UART0 (GPIO1 TX / GPIO3 RX)  ── shared with the CP2102 USB bridge
                    │  GPIO2/13/15 handshake lines (see below)
                    ▼
                 RS-232 level-shifter daughterboard (silkscreen "HW", looks like a MAX3232
                 DB9 module with a 4-pin header)  ──►  female DB9 (DCE / modem side)
```

- **Power:** 5V from micro-USB. The manual says to use a **power-only** supply, because USB data and
  the DB9 share the same UART0 through the CP2102. If a USB host is talking to the board, it takes over the line.
- **Buttons:** NodeMCU **FLASH** (GPIO0) and **RST** (EN). Both are on the dev board.
- **DB9:** female, so it plugs straight into a PC's male serial port. The modem is the DCE.
- **Level shifter (unverified):** the common "HW" MAX3232 DB9 module only has 4 header pins
  (VCC, GND, TXD, RXD), which means it only carries TX/RX. The photo shows a 4-pin header.
  **So DCD/CTS/RTS may not actually reach the DB9 on v3 hardware.** Some of these modules also
  loop back DB9 pins 7↔8 and 1↔4↔6. Check for continuity before you design around hardware handshaking.

## 3. GPIO pin assignments (v3 firmware in this repo)

| Firmware name | ESP8266 GPIO | NodeMCU label | Dir | Function                                                                   | Boot-strap notes                                                    |
|---------------|--------------|---------------|-----|----------------------------------------------------------------------------|---------------------------------------------------------------------|
| (Serial TX)   | GPIO1        | TX            | out | UART0 TX → RS-232 RXD (DB9 pin 2). Also goes to CP2102                     | Prints boot ROM output at 74880 baud                                |
| (Serial RX)   | GPIO3        | RX            | in  | UART0 RX ← RS-232 TXD (DB9 pin 3). Also driven by CP2102                   |                                                                     |
| `SWITCH_PIN`  | GPIO0        | D3            | in  | FLASH button, active LOW. Hold 5 s to force 300 baud                       | Must be HIGH at boot; LOW = UART download mode                      |
| `LED_PIN`     | GPIO16       | D0            | out | Status LED on the NodeMCU board, **active LOW**                            | No interrupts/PWM on GPIO16; used for deep-sleep wake               |
| `DCD_PIN`     | GPIO2        | D4            | out | Carrier Detect output to host (DB9 pin 1)                                  | Must be HIGH at boot. Also drives the ESP-12 blue LED (active LOW)  |
| `RTS_PIN`     | GPIO15       | D8            | out | Modem "ready to receive". Comment says "connect to host's CTS" (DB9 pin 8) | Must be LOW at boot (pulldown on board)                             |
| `CTS_PIN`     | GPIO13       | D7            | in  | Host "ready to receive". Comment says "connect to host's RTS" (DB9 pin 7)  | Free at boot                                                        |

Notes:

- **The handshake pins are named from the host's (DTE) point of view.** `RTS_PIN` is really
  the modem's CTS output, and `CTS_PIN` is really the modem's RTS input. Use DCE names in your
  firmware.
- **Polarity (`AT&P`):** `P_NORMAL` (the default, 1) means the TTL pin is **active LOW**. With the
  MAX3232's inversion, that comes out as asserted (+V) on RS-232. `setCarrier(true)` writes LOW.
  CTS input HIGH = pause TX. `P_INVERTED` (0) flips both.
- **Inconsistency:** `setup()` drives `RTS_PIN` HIGH and comments "ready to receive". Under normal
  polarity that actually means *not ready*. The firmware never changes it after that.
- **Not implemented in hardware or firmware:** DTR (DB9 pin 4), DSR (pin 6) and RI (pin 9).
  "RING" is only sent as a text result code.

### Free GPIOs on a NodeMCU (for new features)

| GPIO   | NodeMCU | Notes                                                           |
|--------|---------|-----------------------------------------------------------------|
| GPIO4  | D2      | Fully free; default I²C SDA                                     |
| GPIO5  | D1      | Fully free; default I²C SCL (README wrongly calls this the LED) |
| GPIO12 | D6      | Free; HSPI MISO                                                 |
| GPIO14 | D5      | Free; HSPI SCLK                                                 |
| A0     | A0      | ADC, 0–3.3V on NodeMCU (divider on board)                       |

GPIO6–11 are used by the SPI flash. Don't use them. You can move UART0 to GPIO13/15
(`Serial.swap()`) to free it from the CP2102, but those pins are already used for handshaking here.

## 4. Serial / UART

- UART0, 8N1, `Serial.begin()` with no config options.
- Supported rates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600 and 115200. The default is 300.
- The rate is stored in EEPROM as an index into that table (byte 111).
- At boot the firmware **blocks until the host sends CR or LF**, or until the FLASH button is held.
  Only then does it print the banner, start WiFi and start the servers.
- Holding FLASH for 5 s at any time: the LED blinks slowly while held, resets the UART to 300 baud,
  then blinks fast until you release it.
- Changing the rate with `AT$SB=` prints a warning, waits 5 s, then re-initialises the UART.

## 5. LED behaviour

- GPIO16, active LOW.
- Off at power-up.
- Blinks at 2 Hz while joining WiFi.
- Solid on when WiFi is connected.
- Toggled on serial or TCP activity.
- The `loop()` code toggles it every pass once 15 ms have passed since the last activity, so in
  practice it flickers or looks dim instead of showing a clean state.

## 6. Non-volatile storage (EEPROM emulation in flash, 781 bytes)

| Addr    | Len | Field                                            | Default                  |
|---------|-----|--------------------------------------------------|--------------------------|
| 0       | 2   | Layout version (0, 2)                            | 0,2                      |
| 2       | 32  | WiFi SSID                                        | ""                       |
| 34      | 63  | WiFi password                                    | ""                       |
| 97      | 1   | IP type (unused)                                 | 'd'                      |
| 98–110  | 4×4 | Static IP/GW/DNS/mask (unused, overlaps 111–113) |                          |
| 111     | 1   | Baud index                                       | 0 (300)                  |
| 112     | 1   | Echo (E)                                         | 1                        |
| 113     | 2   | TCP server port (big-endian)                     | 23                       |
| 115     | 1   | Auto-answer (S0)                                 | 1                        |
| 116     | 1   | Telnet IAC handling (NET)                        | 0                        |
| 117     | 1   | Verbose results (V)                              | 1                        |
| 118     | 1   | PET translate                                    | 0                        |
| 119     | 1   | Flow control (&K) 0=none 1=HW 2=SW               | 2                        |
| 120     | 1   | Pin polarity (&P)                                | 1                        |
| 121     | 1   | Quiet mode (Q)                                   | 0                        |
| 200–699 | 50  | Speed dial 0–9, `host:port`                      | 0–4 preset, 5–9 empty    |
| 700     | 80  | Busy message                                     | "SORRY, SYSTEM IS BUSY…" |

## 7. Firmware features (summary)

- **Hayes-style AT command set:**
  - Commands: `AT`, `ATDT`/`ATDP`/`ATDI host[:port]`, `ATDSn` (speed dial), `ATA`, `ATH`, `ATO`,
    `+++` (with 1 s guard time), `ATZ`, `AT&F`, `AT&W`, `AT&V`, `ATI`, `ATE`, `ATV`, `ATQ`, `AT&K`,
    `AT&P`, `ATS0=`, `AT&Zn=`.
  - Settings: `AT$SSID=`, `AT$PASS=`, `AT$SB=`, `AT$BM=`, `AT$SP=`.
  - WiFi on/off: `ATC0`/`ATC1`.
  - Network and misc: `ATIP?`, `ATGET<url>` (raw HTTP GET), `ATNETn` (telnet IAC), `ATPET=n`,
    `ATHEX=n`, `AT$RB` (reboot), `AT?`/`ATHELP`.
  - Most commands also take a `?` query form.
- **Outbound calls:** TCP client to any host:port. Port 23 is the default. Nagle is disabled.
- **Inbound calls:** TCP server on port 23 (configurable).
  - Sends RING results, or auto-answers.
  - Sends the busy message to extra callers.
- **Telnet:** optional IAC handling. It refuses every DO, accepts every WILL, and escapes 0xFF.
- **PPP server (`ATDTPPP`):**
  - Uses lwIP PPPoS over the serial line with NAPT to WiFi, so a vintage PC can get real IP networking.
  - The ESP's own IP is used as the local address, and the peer gets `192.168.240.2`.
  - DNS is handed out from WiFi's DNS servers. No authentication.
- **HTTP status page** on port 80:
  - `/` shows WiFi and call status.
  - `/ath` hangs up.
- **mDNS** name `C64WiFi`.
- **PETSCII helpers** for C64/PET terminals. The tables are defined but unused. There is a
  high-bit strip and case fixups.
- **Flow control:**
  - Hardware mode only gates TCP→serial writes on the CTS input.
  - **Software (XON/XOFF), the default, isn't implemented at all.**

## 8. Hardware-relevant quirks to fix in your firmware

1. The pin names don't match the RS-232 roles, and the RTS/CTS polarity is inconsistent (see §3).
2. XON/XOFF, the default mode, does nothing. The firmware also never asserts or deasserts its own "ready" line.
3. UART0 is shared with the CP2102, so debug output and modem data can't be separated. On the
   ESP8266, `Serial1` (GPIO2, TX only) could carry a debug log, but GPIO2 is the DCD line here.
4. GPIO15 (the "RTS" output) is a boot-strap pin. The level shifter must not pull it HIGH during
   reset, or the board won't boot.
5. The boot wait for CR/LF means the modem isn't on WiFi until the host sends something.
6. The EEPROM layout overlaps (static-IP fields vs. baud/echo/port). The baud bounds check uses `>` instead of `>=`.

