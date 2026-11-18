#!/usr/bin/env python3
"""The Mac's end of the bridge's USB serial port (SDBRIDGE.UF2).

    link.py serve PORT LOG [FILE]
        Write everything the bridge says into LOG, each line stamped with
        the seconds since the start, and whenever FILE (a program, say
        sdk/out/linkbench-192.bin) is new or changed, send it to the
        bridge, which then has it on its card as LINKBENC.BIN: the SD
        loader runs it, and again at RETURN, with no card to swap.  Goes
        on when the port drops off and comes back.
    link.py put PORT FILE
        Send FILE once and print the bridge's answer.
    link.py results LOG
        What the last run of sdk/examples/linkbench sent through the
        bridge (CMD16 arguments in its log): the screen's text, and the
        first bad sector of each test that had one, word by word.
    link.py bootsel PORT
        Restart the board into its USB drive (RPI-RP2) for a new UF2:
        the port opened at 1200 baud, as the Pico SDK's USB serial port
        takes it.

PORT is the board's serial device (/dev/cu.usbmodem... on a Mac)."""
import os
import sys
import termios
import time


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def open_port(dev, speed=termios.B115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    attr = termios.tcgetattr(fd)
    attr[0] = 0
    attr[1] = 0
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0
    attr[4] = attr[5] = speed
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 1                    # reads wait 0.1 s at most
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    return fd


def send(fd, data):
    os.write(fd, b'PUT %d %08x\n' % (len(data), fnv(data)))
    for i in range(0, len(data), 1024):
        os.write(fd, data[i:i + 1024])


def serve(dev, log, path):
    start = time.time()
    sent = None
    with open(log, 'a', buffering=1) as out:
        def note(text):
            out.write('%8.3f --- %s\n' % (time.time() - start, text))

        note('capture of %s started %s' % (dev, time.strftime('%Y-%m-%d %H:%M:%S')))
        while True:
            try:
                fd = open_port(dev)
            except OSError:
                time.sleep(0.2)
                continue
            note('port open')
            sent = None                            # the board may have restarted
            buf = b''
            try:
                while True:
                    if path:
                        try:
                            st = os.stat(path)
                            key = (st.st_mtime_ns, st.st_size)
                        except OSError:
                            key = None
                        if key and key != sent and time.time() - st.st_mtime > 0.5:
                            data = open(path, 'rb').read()
                            send(fd, data)
                            sent = key
                            note('sent %s: %d bytes, sum %08x' % (path, len(data), fnv(data)))
                    buf += os.read(fd, 4096)
                    while b'\n' in buf:
                        line, buf = buf.split(b'\n', 1)
                        out.write('%8.3f %s\n' % (time.time() - start, line.rstrip(b'\r').decode('utf-8', 'replace')))
            except OSError as e:
                note('port lost (%s); waiting for it' % e.strerror)
                try:
                    os.close(fd)
                except OSError:
                    pass


def put(dev, path):
    data = open(path, 'rb').read()
    fd = open_port(dev)
    send(fd, data)
    buf, t = b'', time.time()
    while time.time() - t < 5:
        buf += os.read(fd, 4096)
        for line in buf.split(b'\n'):
            if line.startswith(b'put:'):
                print(line.decode('utf-8', 'replace').rstrip())
                return 0 if line.rstrip().endswith(b'ok') else 1
    print('no answer from the bridge', file=sys.stderr)
    return 1


def pattern(j):
    """Word j of LINKTEST.BIN (tools/linktest.py)."""
    x = ((j + 1) * 0x9E3779B1) & 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & 0xFFFFFFFF
    x ^= x >> 13
    return x


def results(log):
    import re
    args = [int(m.group(1), 16) for m in re.finditer(r'CMD16 ([0-9a-f]{8})', open(log, errors='replace').read())]
    starts = [i for i, a in enumerate(args) if a == 0x4C4B0001]
    if not starts:
        print('no run in the log')
        return 1
    a = args[starts[-1] + 1:]
    if 0x4C4B00FF in a:
        a = a[:a.index(0x4C4B00FF)]
    else:
        print('(no end: the run was cut short, or is still sending)')
    items, dumps, text, i = {}, {}, None, 0
    while i < len(a):
        k = a[i]
        if k == 0x7E00FD00:
            j = a.index(0x7E00FDFF, i) if 0x7E00FDFF in a[i:] else len(a)
            text = b''.join(w.to_bytes(4, 'big') for w in a[i + 1:j]).rstrip(b'\0').decode('ascii', 'replace')
            i = j + 1
        elif k >> 24 == 0x7E and k & 0xFF == 0x40:
            dumps[(k >> 8) & 0xFF] = a[i + 1:i + 129]
            i += 129
        elif k >> 24 == 0x7E and (k >> 8) & 0xFF == 0xFF and k & 0x80:
            i += 6
        elif k >> 24 == 0x7E:
            items.setdefault((k >> 8) & 0xFF, {})[k & 0xFF] = a[i + 1] if i + 1 < len(a) else None
            i += 2
        else:
            i += 1
    if text is not None:
        print(text)
    for t, w in sorted(dumps.items()):
        s = items.get(t, {}).get(6, 0)                # the first bad sector
        wrong = [(k, w[k], pattern(s * 128 + k)) for k in range(len(w)) if w[k] != pattern(s * 128 + k)]
        print('test %d, sector %d: %d of 128 words wrong' % (t, s, len(wrong)))
        for k, got, want in wrong[:12]:
            print('   word %3d  %08x  wanted %08x' % (k, got, want))
    return 0


def bootsel(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    attr = termios.tcgetattr(fd)
    attr[4] = attr[5] = termios.B1200
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    os.close(fd)
    return 0


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) in (3, 4) and a[0] == 'serve':
        serve(a[1], a[2], a[3] if len(a) == 4 else None)
    elif len(a) == 3 and a[0] == 'put':
        sys.exit(put(a[1], a[2]))
    elif len(a) == 2 and a[0] == 'results':
        sys.exit(results(a[1]))
    elif len(a) == 2 and a[0] == 'bootsel':
        sys.exit(bootsel(a[1]))
    else:
        sys.exit(__doc__)
