#!/usr/bin/env python3
"""Test termshare-client SGR-aware truncation against a mock server.

Runs termshare-client (attach 1) in a pty against a mock Unix socket
server that sends coloured SYNC/UPDATE rows, then checks the client's
rendered output:
  - coloured text appears intact (RED, plain, etc.)
  - a long coloured row is truncated to the grid width in VISIBLE
    cells, not in bytes: first 20 visible chars appear, later chars
    do not, and no escape sequence is cut in the middle
  - every CSI sequence in the output is complete (ends with a final
    byte in @..~ before the next ESC or end of output)
Usage: mock_colour_client.py <path-to-termshare-client>
Exit 0 on pass, 1 on fail.
"""
import os, pty, sys, time, subprocess, select, fcntl, termios, struct, socket, re

CLIENT = sys.argv[1]
RUNTIME = "/tmp/termshare-colour-test"
SOCK = os.path.join(RUNTIME, "termshare", "share.sock")
os.makedirs(os.path.dirname(SOCK), exist_ok=True)
try:
    os.unlink(SOCK)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(SOCK)
srv.listen(5)
os.chmod(SOCK, 0o600)

def read_frame(conn):
    hdr = b""
    while len(hdr) < 4:
        chunk = conn.recv(4 - len(hdr))
        if not chunk:
            return None
        hdr += chunk
    n = struct.unpack(">I", hdr)[0]
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def send(conn, payload):
    conn.sendall(struct.pack(">I", len(payload)) + payload)

def server_thread():
    conn, _ = srv.accept()
    send(conn, b"WELCOME 1\n")
    while True:
        f = read_frame(conn)
        if f is None:
            break
        if f == b"LIST\n":
            send(conn, b"LIST\nWINDOW 1\tmain\nTAB 1\t7\t1\tmain\nEND\n")
        elif f.startswith(b"ATTACH "):
            # grid 20 cols, 3 rows; row 1 is a long coloured row
            # (26 visible cells) that must truncate to 20 visible cells.
            send(conn, b"SYNC 20 3 0 0\nTABBAR 0\tmain\n"
                       b"\x1b[0m\x1b[38;2;192;0;0mRED\x1b[0m plain\x1b[0m\n"
                       b"\x1b[0m\x1b[38;2;0;192;0mABCDEFGHIJKLMNOPQRSTUVWXYZ\x1b[0m\n"
                       b"plainrow\n")
            time.sleep(0.5)
            send(conn, b"UPDATE 0 0\nROW 1\t\x1b[0m\x1b[38;2;0;0;192m0123456789ABCDEFGHIJKLMNOP\x1b[0m\n")
        elif f == b"DETACH\n":
            break
        # RESIZE / INPUT: ignore
    try:
        conn.close()
    except OSError:
        pass

import threading
t = threading.Thread(target=server_thread, daemon=True)
t.start()
time.sleep(0.5)

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
proc = subprocess.Popen([CLIENT, "attach", "1"], stdin=slave, stdout=slave,
                        stderr=slave, env={**os.environ, "XDG_RUNTIME_DIR": RUNTIME},
                        close_fds=True)
os.close(slave)
out = b""
def drain(seconds):
    global out
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
drain(2.0)
try:
    os.write(master, b"\x1d")  # Ctrl+] detach
except OSError:
    pass
drain(1.0)
try:
    proc.wait(timeout=3)
except Exception:
    proc.kill()
try:
    os.close(master)
except OSError:
    pass

failures = []
def check(ok, label):
    print(("PASS: " if ok else "FAIL: ") + label)
    if not ok:
        failures.append(label)

print("--- captured output (repr, last 2000 bytes) ---")
print(repr(out[-2000:]))
check(b"RED" in out and b"plain" in out, "coloured SYNC row renders RED/plain")
# Long green row: first 20 visible chars must appear, last chars must not
check(b"ABCDEFGHIJKLMNOPQRST" in out, "long coloured row truncates to 20 visible chars (first 20 present)")
# "UVWXYZ" must not appear as rendered grid text; it could appear only
# if truncation counted bytes or cut incorrectly.  Note: the raw row
# is never echoed by the client, only its rendering, so absence is a
# real signal.
check(b"UVWXYZ" not in out, "long coloured row tail (UVWXYZ) not rendered after truncation")
# Update row: first 20 of "0123456789ABCDEFGHIJKLMNOP" = 0123456789ABCDEFGHIJ
check(b"0123456789ABCDEFGHIJ" in out, "UPDATE coloured row truncates to 20 visible chars")
# Note: "KLMNOP" alone also appears inside the SYNC row
# (ABCDEFGHIJKLMNOPQRST), so the tail check must be the full
# UPDATE string, which must never appear rendered in full.
check(b"0123456789ABCDEFGHIJKLMNOP" not in out,
      "UPDATE coloured row tail not rendered")
# CSI completeness: every ESC[ sequence must reach a final byte @..~
# before the next ESC or the end of output.
i = 0
incomplete = []
while True:
    j = out.find(b"\x1b[", i)
    if j < 0:
        break
    k = j + 2
    while k < len(out) and not (0x40 <= out[k] <= 0x7e):
        k += 1
    if k >= len(out):
        incomplete.append(out[j:j+20])
        break
    i = k + 1
check(not incomplete, "no truncated CSI sequence in client output (found %r)" % (incomplete[:3],))
# A reset must follow the truncated rows (client appends one per row).
check(out.count(b"\x1b[0m") >= 3, "reset sequences present after grid rows")

print("CLIENT COLOUR TEST RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
sys.exit(1 if failures else 0)
