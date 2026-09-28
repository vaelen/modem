# modem

Arduino firmware for an ESP8266 that emulates a modem and allows incomming and outgoing connections over WiFi. It also has the ability to block IP addresses from connecting. This can be used to reduce the number of spam connections to the server.

Features:
- Listens on a TCP/IP port for incomming callers. (doesn't support 'RING', always auto-answers.)
- The computer can dial out to any `host[:port]`.
- `+++` escapes to command mode.
- Abusive callers can be banned by IP, and HTTP scanners are banned automatically.

Otherwise this is intentionally a very bare-bones emulator.  
There's no PPP, SLIP, telnet protocol handling or web interface. Everything is
configured with AT commands.

**Note:** This firmware was designed for and tested on the 'OldNet v6' Wifi modem.

## Hardware

| DB9 pin    | Signal            | ESP8266                 |
|------------|-------------------|-------------------------|
| 2          | TX out            | GPIO15 (UART0, swapped) |
| 3          | RX in             | GPIO13 (UART0, swapped) |
| 7          | RTS in            | GPIO14                  |
| 8          | CTS out           | GPIO12                  |
| 1, 4, 6, 9 | DCD, DTR, DSR, RI | not wired on the board  |

- **FLASH button (GPIO0):** resets the DB9 to 19200 baud (see below).
- **Status LED:** the ESP8266's own LED (GPIO16).
- **DB9 defaults:** 19200 8N1.
- **USB port:** used only for flashing. After boot, the UART is swapped over to the DB9.
- **Hosts that wait for DCD or DSR** need a cable that loops DTR to DCD and DSR.

[HARDWARE.md](HARDWARE.md) has the details of the board. All pin assignments live in
[include/board.h](include/board.h).

## Build and flash

Needs [PlatformIO](https://platformio.org/).

```sh
pio run                                     # build
pio run -t upload --upload-port /dev/ttyUSB0   # flash over the ESP8266's USB port
```

## First-time setup

Connect a terminal to the DB9 at 19200 8N1 and type:

```
AT$SSID=My Network
AT$PASS=my password
AT&W
ATI
```

- **`ATI` status:** shows `WIFI: CONNECTED <ssid> <ip>` once the modem is on the network. Until then it
  shows `WIFI: DISCONNECTED <ssid> (<reason>)`.
- **Callers:** they reach the modem at that IP, on port 23 by default.
- **Networks:** only 2.4 GHz WPA2 networks work (an ESP8266 limit).

## AT commands

Commands can be chained (`AT+BAN;H`). Letters can be upper or lower case. Anything unrecognised is
accepted with `OK`, so terminal init strings work.

| Command                | Effect                                                                 |
|------------------------|------------------------------------------------------------------------|
| `ATD[T\|P]host[:port]` | dial out (port 23 by default); `CONNECT <baud>` or `NO CARRIER`        |
| `+++`                  | back to command mode during a call (0.5 s of silence before and after) |
| `ATO`                  | return to the call                                                     |
| `ATH`                  | hang up                                                                |
| `AT+BAN`               | ban the current caller (from command mode during an inbound call)      |
| `AT+BAN?`              | list the ban table: `+BAN: <ip>,<strikes>,<minutes left>`              |
| `AT+BAN=0`             | clear the ban table                                                    |
| `ATE0` / `ATE1`        | command echo off / on (default on)                                     |
| `AT&K0` / `AT&K3`      | flow control off / RTS-CTS (default off)                               |
| `AT+IPR=<baud>`        | DB9 speed: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200    |
| `AT$SSID=<name>`       | WiFi network name                                                      |
| `AT$PASS=<pass>`       | WiFi password (never shown)                                            |
| `AT$PORT=<port>`       | port callers connect to (default 23)                                   |
| `AT...?`               | show a setting: `AT+IPR?`, `AT$SSID?`, `AT$PORT?`                      |
| `ATI`                  | status: WiFi, IP, port, baud, bans                                     |
| `AT&W`                 | save the settings to flash                                             |
| `ATZ`                  | hang up, then reload the saved settings                                |
| `AT&F`                 | factory defaults, except the WiFi settings and port                    |

- **Saving:** settings take effect immediately but only reach flash with `AT&W`. `ATZ` or a reboot
  undoes unsaved changes.
- **What's saved:** SSID, password, baud, port, echo and flow control.
- **`ATZ` and WiFi:** `ATZ` only reconnects WiFi if the saved WiFi settings differ, so terminal
  programs that send it at startup don't knock the modem off the network.
- **Full factory reset:** `AT$SSID=`, `AT$PASS=`, `AT&F`, `AT&W`.
- **Values:** `$SSID=` and `$PASS=` take the rest of the line exactly as typed, including spaces and
  `;`.

## Calls

- **Incoming:** the modem waits up to 1 s for the caller's first 4 bytes before answering.
  - `GET ` (an HTTP scanner): the caller is banned and dropped. The computer never sees it.
  - Anything else: `CONNECT <baud>`, then those bytes are passed on.
  - Banned IPs are dropped silently.
  - A caller arriving while a call is up gets `BUSY, PLEASE TRY AGAIN LATER`.
- **Outgoing:** `ATDT bbs.example.com:23`. Dialing blocks for up to 10 s while it connects.
- **End of call:** `NO CARRIER` when either side hangs up.

## IP bans

- **Durations:** each strike bans the IP for longer: 5 minutes, then 1 hour, then 24 hours.
- **Reset:** a day without strikes starts the count over.
- **Storage:** the table holds 256 IPs in RAM and is cleared on reboot.

## Flow control

With `AT&K3`:
- **RTS (pin 7):** when the host drops RTS, the modem stops sending to it.
- **CTS (pin 8):** the modem drops CTS when its 1 KB receive buffer is ¾ full, and raises it again at ¼.

With `AT&K0`, CTS stays asserted and RTS is ignored.

## Status LED

| LED              | Meaning                                                     |
|------------------|-------------------------------------------------------------|
| solid on         | WiFi connected                                              |
| 1 blink/s        | trying to connect                                           |
| 4 blinks/s       | WiFi error: network not found, wrong password, failed, lost |
| 10 blinks in 1 s | FLASH-button baud reset                                     |
| off              | no WiFi configured                                          |

## Lost the baud rate?

If you set a speed your terminal can't use, hold the FLASH button for 3 seconds once the board has
booted. The LED blinks rapidly and the DB9 goes back to 19200. Send `AT&W` to keep it.

Don't hold FLASH while powering on or pressing RST: that starts the ESP8266's bootloader instead.

## Tests

- **Unit tests** (tokenizer and ban table), run on the PC:

  ```sh
  pio test -e native
  ```

- **End-to-end tests** on the real board: they drive the DB9 through a USB-RS232 adapter and call the
  modem over WiFi. The modem needs WiFi saved first, and this machine must accept connections on port
  2324.

  ```sh
  pip install pyserial
  tools/hwtest.py --port /dev/ttyUSB1 --modem <modem ip> --me <this machine's ip>
  ```

  Add `--no-cts` if your adapter doesn't report CTS (many CH340 cables don't).

## Porting

Everything board-specific is in [include/board.h](include/board.h):
- the serial port wired to the DB9 (`Modem`)
- the pins
- the WiFi header
- the functions that start the port and change its baud rate

To port, add a block for your board there. `main.cpp` doesn't name `Serial` or any GPIO. Leave out any
pin your board doesn't have, and the code that uses it compiles out.

[DESIGN.md](DESIGN.md) explains how the firmware works.

## License

MIT, see [LICENSE](LICENSE).
