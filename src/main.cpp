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
#define RING_MS 6000            // one RING every 6 s, like a US phone line
#define RING_MAX 10             // under S0=0, a caller nobody answers is dropped after this many
#define CONFIG_MAGIC 0x314D444DUL   // "MDM1" little-endian

struct Config {
    uint32_t magic;
    char ssid[33];
    char pass[65];
    uint32_t baud;
    uint16_t port;
    uint8_t echo;
    uint8_t flow;               // 0 = none, 3 = RTS/CTS
    uint8_t s0;                 // auto-answer on this ring, 0 = never (the host sends ATA)
};

enum State { IDLE, SCREENING, RINGING, ONLINE, ONLINE_CMD };  // on-hook, caller not yet announced, RING, data, after +++

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
static uint32_t ledFlashStart = 0;      // when a FLASH-button reset started its 1 s of blinks
static uint8_t tcpOut[160];             // host bytes TCP hasn't taken yet (128 read + held '+')
static int ntcpOut = 0;
static uint32_t lastModemMs = 0;        // when the last byte came from the host
static uint8_t early[4];                // an answering caller's first bytes
static int nearly = 0;
static uint32_t answerMs = 0;           // when the caller being screened connected
static int rings = 0;                   // RINGs sent for the ringing caller
static uint32_t ringMs = 0;             // when the last RING went out

static void defaults(Config &c) {
    memset(&c, 0, sizeof c);
    c.magic = CONFIG_MAGIC;
    c.baud = 19200;
    c.port = 23;
    c.echo = 1;
    c.s0 = 1;
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
    if (c.s0 == 0xFF) c.s0 = 1;         // erased flash: saved before S0 existed (so S0 tops out at 254)
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
        modemBaud(cfg.baud);
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

// ATA or the S0th RING.
static void answer() {
    nline = 0;
    goOnline();
    Modem.write(early, nearly);
}

// The call is over: on-hook, then res. ATH/ATZ pass nullptr and get the line's OK, as on a Hayes.
static void hangup(const char *res = "NO CARRIER") {
    client.stop();
    callerIp = 0;
    nheld = 0;
    ntcpOut = 0;
    nline = 0;
    state = IDLE;
    if (res) result(res);
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
    client.setTimeout(0);               // write() must never block loop(): see tcpFlush()
    callerIp = 0;
    goOnline();
}

// Why WiFi is down, or nullptr while it's still trying (the ESP8266 then
// reports plain DISCONNECTED or IDLE). ATI shows it; the LED blinks fast on it.
static const char *wifiError() {
    switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL: return "NETWORK NOT FOUND";
    case WL_CONNECT_FAILED: return "CONNECT FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION LOST";
#ifdef ARDUINO_ARCH_ESP8266
    case WL_WRONG_PASSWORD: return "WRONG PASSWORD";
#endif
    default: return nullptr;
    }
}

static void showInfo() {
    info("MODEM " VERSION);
    if (!cfg.ssid[0]) info("WIFI: NOT CONFIGURED");
    else if (WiFi.status() == WL_CONNECTED)
        info("WIFI: CONNECTED %s %s", cfg.ssid, WiFi.localIP().toString().c_str());
    else info("WIFI: DISCONNECTED %s (%s)", cfg.ssid, wifiError() ? wifiError() : "TRYING");
    info("PORT: %u", cfg.port);
    info("BAUD: %lu", (unsigned long)cfg.baud);
    info("FLOW: %s", cfg.flow == 3 ? "RTS/CTS" : "NONE");
    info("S0: %u", cfg.s0);
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
    char *start = line;                 // noise that had an 'A' in it: parse from the first "AT"
    while (start[0] && start[1] && !((start[0] | 0x20) == 'a' && (start[1] | 0x20) == 't')) start++;
    int n = atParse(start[1] ? start : line, cmds);
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
            case 'A':                   // nothing ringing: NO CARRIER, as when no carrier answers
                if (state == ONLINE_CMD) { err = true; break; }
                apply(next);
                if (state == RINGING) answer();
                else result("NO CARRIER");
                return;
            case 'D':
                if (state != IDLE) { err = true; break; }
                apply(next);
                dial(c.arg);            // prints its own result
                return;
            case 'H':
                if (state == ONLINE_CMD) hangup(nullptr);
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
                if (state == ONLINE_CMD) hangup(nullptr);
                loadProfile(next);
                break;
            }
        } else if (c.prefix == '&') {
            if (c.name[0] == 'K') {
                if (c.op == '?') info("&K: %u", next.flow);
                else if (v == 0 || v == 3) next.flow = (uint8_t)v;
                else err = true;
            } else if (c.name[0] == 'W') {
                save = true;
            } else if (c.name[0] == 'F') {  // keeps WiFi and port: AT&F is in terminal init strings
                Config f;
                defaults(f);
                memcpy(f.ssid, next.ssid, sizeof f.ssid);
                memcpy(f.pass, next.pass, sizeof f.pass);
                f.port = next.port;
                next = f;
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
        } else if (c.prefix == 'S' && c.num == 0) {
            if (c.op == '?') info("%03u", next.s0);
            else if (c.op == '=' && c.val <= 254) next.s0 = (uint8_t)c.val;
            else if (c.op == '=') err = true;
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
        // Like a Hayes modem, skip everything before the "A" of "AT": line
        // noise (a baud mismatch, boot glitches) can't eat the next command.
        if (nline == 0 && ch != 'A' && ch != 'a') return;
        if (nline < (int)sizeof line - 1) line[nline++] = ch;
        else lineOverflow = true;
    }
}

// Hand queued host bytes to TCP. The call's timeout is 0, so write() never
// waits: lwIP can refuse bytes (its segment queue fills before its buffer
// does), and whatever it refuses stays here for the next pass.
static void tcpFlush() {
    if (ntcpOut == 0) return;
    int w = (int)client.write(tcpOut, ntcpOut);
    memmove(tcpOut, tcpOut + w, ntcpOut - w);
    ntcpOut -= w;
}

// Data mode, host → TCP. A '+' after >= GUARD_MS of silence starts holding
// up to three; anything else releases them as data. checkGuard() decides.
// While TCP has refused bytes, host bytes stay in the UART, where CTS sees them.
static void pumpToTcp() {
    uint8_t in[128];
    tcpFlush();
    if (ntcpOut) return;
    int n = min(Modem.available(), (int)sizeof in);
    if (n <= 0) return;
    n = Modem.readBytes(in, n);
    uint32_t t = millis();
    for (int i = 0; i < n; i++) {
        bool quiet = t - lastModemMs >= GUARD_MS;
        lastModemMs = t;
        if (in[i] == '+' && nheld < 3 && (nheld > 0 || quiet)) {
            nheld++;
            continue;
        }
        while (nheld > 0) { tcpOut[ntcpOut++] = '+'; nheld--; }
        tcpOut[ntcpOut++] = in[i];
    }
    tcpFlush();
}

static void checkGuard() {
    // Bytes still queued behind the '+' arrived inside the guard time, even if
    // a stalled TCP send kept us from reading them: not an escape yet.
    if (nheld == 0 || Modem.available() || millis() - lastModemMs < GUARD_MS) return;
    if (nheld == 3) {
        state = ONLINE_CMD;
        nline = 0;
        result("OK");
    } else {
        while (nheld > 0) { tcpOut[ntcpOut++] = '+'; nheld--; }  // lone '+' or '++': just data
        tcpFlush();
    }
    nheld = 0;
}

static bool hostReady() {             // may we send to the host?
#ifdef PIN_RTS_IN
    if (cfg.flow == 3 && digitalRead(PIN_RTS_IN) != PIN_ASSERTED) return false;
#endif
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

// Drop a caller the host never heard about.
static void dropCaller() {
    client.stop();
    callerIp = 0;
    state = IDLE;
}

// Answer, refuse (banned or redialing too fast: silently) or send BUSY.
static void acceptCaller() {
    WiFiClient c = server.accept();
    if (!c) return;
    uint32_t ip = c.remoteIP();
    if (banCheck(ip, millis()) || banRedial(ip, millis())) { c.stop(); return; }
    if (state != IDLE) {
        c.print("BUSY, PLEASE TRY AGAIN LATER\r\n");
        c.stop();
        return;
    }
    client = c;
    client.setNoDelay(true);
    client.setTimeout(0);               // write() must never block loop(): see tcpFlush()
    callerIp = ip;
    nearly = 0;
    answerMs = millis();
    state = SCREENING;
}

// A new caller's first 4 bytes decide: "GET " is an HTTP scanner, banned
// and dropped. So is a caller that hangs up first. Anything else, or
// ANSWER_MS without 4 bytes, rings, and the bytes go to the host on answer.
static void checkScreen() {
    while (nearly < 4 && client.available()) early[nearly++] = (uint8_t)client.read();
    if (nearly == 4 && !memcmp(early, "GET ", 4)) {
        banStrike(callerIp, millis());
        dropCaller();
        return;
    }
    if (!client.connected() && !client.available()) { dropCaller(); return; }
    if (nearly < 4 && millis() - answerMs < ANSWER_MS) return;
    rings = 0;
    ringMs = millis() - RING_MS;        // first RING now
    state = RINGING;
}

// RING every RING_MS until S0 rings or ATA answer. A caller that hangs up
// stops it; under S0=0 one nobody answers is dropped, so it can't hold the line.
static void checkRing() {
    if (!client.connected() && !client.available()) { dropCaller(); return; }
    if (millis() - ringMs < RING_MS) return;
    if (cfg.s0 == 0 && rings >= RING_MAX) { dropCaller(); return; }
    ringMs = millis();
    rings++;
    result("RING");
    if (cfg.s0 && rings >= cfg.s0) answer();   // >=: ATS0 may lower it mid-ring
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
        ledFlashStart = millis() | 1;   // | 1: 0 means none
        done = true;
    }
#endif
}

// Status LED: solid = WiFi up, 1 blink/s = trying, 4/s = WiFi error, 10 in
// 1 s = FLASH-button baud reset, off = no WiFi configured.
// ponytail: frozen while a dial blocks (up to 10 s); async dial fixes both.
static void updateLed() {
#ifdef PIN_LED
    uint32_t t = millis();
    bool on;
    if (ledFlashStart && t - ledFlashStart < 1000) on = (t - ledFlashStart) % 100 < 50;
    else if (!cfg.ssid[0]) on = false;
    else if (WiFi.status() == WL_CONNECTED) on = true;
    else if (wifiError()) on = t % 250 < 125;
    else on = t % 1000 < 500;
    digitalWrite(PIN_LED, on ? PIN_LED_ON : !PIN_LED_ON);
#endif
}

void setup() {
#ifdef PIN_LED
    digitalWrite(PIN_LED, !PIN_LED_ON);
    pinMode(PIN_LED, OUTPUT);
#endif
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
    if (state == SCREENING) checkScreen();
    if (state == RINGING) checkRing();
    if (state == ONLINE) {
        pumpToModem();
        pumpToTcp();
        checkGuard();
    } else {
        while (Modem.available() && state != ONLINE) commandByte((char)Modem.read());  // IDLE, SCREENING, RINGING, ONLINE_CMD
        if (state == ONLINE_CMD) {      // remote data in command mode is dropped, as in modem.c
            uint8_t buf[128];
            while (client.available()) client.read(buf, sizeof buf);
        }
    }
    if ((state == ONLINE || state == ONLINE_CMD) && !client.connected() && !client.available()) hangup();
    updateCts();
    updateLed();
}
