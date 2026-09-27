#!/usr/bin/env python3
# Copyright 2026, Andrew C. Young <andrew@vaelen.org>
# SPDX-License-Identifier: MIT
"""End-to-end test against the real modem: pyserial drives the DB9 through a
USB-RS232 adapter, sockets on this machine play the callers and dialed peers.

Before running: the modem has WiFi configured and saved (AT$SSID=, AT$PASS=,
AT&W), is at 19200 baud with &K0 saved, and can reach this machine on
PEER_PORT. Sections run in order; a failed assert stops the run.

  pip install pyserial
  tools/hwtest.py --port /dev/ttyUSB0 --modem 192.168.1.50 --me 192.168.1.10 [config] [calls] [flow]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial device wired to the DB9")
    ap.add_argument("--modem", required=True, help="the modem's IP (ATI)")
    ap.add_argument("--me", required=True, help="this machine's IP, reachable from the modem")
    ap.add_argument("sections", nargs="*", default=["config", "calls", "flow"])
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
