#!/usr/bin/env python3
"""The bridge's PIO programs (sdbridge/src/sd.pio) run against an SD host's waveforms,
one system clock at a time, from the instructions pioasm made of them:

    python3 -I sdbridge/tests/piosim.py sdbridge/out/sd.pio.h

A model of an RP2040 state machine as far as these programs need it (JMP, WAIT on
GPIO, IN, OUT, PUSH/PULL, SET; autopush and autopull, shifting left; the two cycles of
the input synchronisers; no side-set), and of a host that changes CMD after a falling
edge of its clock and reads CMD and DAT0 at a rising one.  The host's clock is given in
system clocks a period: 200 MHz against 16.9 MHz (the 192's stock setting), 22.6 and
33.9 MHz, and the 264 kHz of a card's start."""
import re
import sys

SYNC = 2   # cycles of the input synchronisers


def load(path):
    text = open(path).read()
    programs = {}
    for m in re.finditer(r"static const uint16_t (\w+)_program_instructions\[\] = \{(.*?)\};", text, re.S):
        words = [int(h, 16) for h in re.findall(r"0x([0-9a-f]{4})", m.group(2))]
        programs[m.group(1)] = words
    defines = {k: int(v) for k, v in re.findall(r"#define (\w+) (\d+)u?\b", text)}
    return programs, defines


class SM:
    def __init__(self, code, pin, start=0):
        self.code, self.pin, self.pc = code, pin, start
        self.x = self.y = 0
        self.isr, self.isr_n = 0, 0
        self.osr, self.osr_n = 0, 32
        self.tx, self.rx = [], []
        self.out_val, self.out_dir = 1, 0
        self.delay = 0

    def step(self, seen):
        """One cycle; `seen` the pin levels through the synchronisers."""
        if self.delay:
            self.delay -= 1
            return
        w = self.code[self.pc]
        op, arg = w >> 13, w & 0xff
        delay = (w >> 8) & 0x1f
        nxt = (self.pc + 1) % len(self.code)
        if op == 0:      # JMP
            cond, addr = arg >> 5, arg & 0x1f
            take = {0: lambda: True, 1: lambda: self.x == 0, 6: lambda: seen[self.pin] == 1}.get(cond)
            if cond == 2:
                take = self.x != 0
                self.x = (self.x - 1) & 0xffffffff
            elif take is None:
                raise ValueError("jmp condition %d" % cond)
            else:
                take = take()
            self.pc = addr if take else nxt
        elif op == 1:    # WAIT
            pol, src, idx = arg >> 7, (arg >> 5) & 3, arg & 0x1f
            assert src == 0, "wait source"
            if seen[idx] != pol:
                return   # stalled
            self.pc = nxt
        elif op == 2:    # IN
            src, n = arg >> 5, (arg & 0x1f) or 32
            if self.isr_n >= 32:
                return
            v = {0: seen[self.pin], 3: 0}[src]
            self.isr = ((self.isr << n) | (v & ((1 << n) - 1))) & 0xffffffff
            self.isr_n += n
            if self.isr_n >= 32:     # autopush
                if len(self.rx) >= 4:
                    raise AssertionError("RX FIFO full")
                self.rx.append(self.isr)
                self.isr, self.isr_n = 0, 0
            self.pc = nxt
        elif op == 3:    # OUT
            dst, n = arg >> 5, (arg & 0x1f) or 32
            if self.osr_n >= 32:     # autopull
                if not self.tx:
                    return
                self.osr, self.osr_n = self.tx.pop(0), 0
            v = self.osr >> (32 - n)
            self.osr = (self.osr << n) & 0xffffffff
            self.osr_n += n
            if dst == 0:
                self.out_val = v & 1
            elif dst == 1:
                self.x = v
            else:
                raise ValueError("out destination %d" % dst)
            self.pc = nxt
        elif op == 4:    # PUSH / PULL
            if arg & 0x80:
                if not self.tx:
                    return
                self.osr, self.osr_n = self.tx.pop(0), 0
            else:
                self.rx.append(self.isr)
                self.isr, self.isr_n = 0, 0
            self.pc = nxt
        elif op == 7:    # SET
            dst, v = arg >> 5, arg & 0x1f
            if dst == 0:
                self.out_val = v & 1
            elif dst == 1:
                self.x = v
            elif dst == 4:
                self.out_dir = v & 1
            else:
                raise ValueError("set destination %d" % dst)
            self.pc = nxt
        else:
            raise ValueError("instruction %04x" % w)
        self.delay = delay


def crc7(b):
    c = 0
    for byte in b:
        for i in range(7, -1, -1):
            top = (c >> 6) & 1
            c = (c << 1) & 0x7f
            if top ^ ((byte >> i) & 1):
                c ^= 0x09
    return c


def command(index, arg):
    b = [0x40 | index, arg >> 24 & 0xff, arg >> 16 & 0xff, arg >> 8 & 0xff, arg & 0xff]
    return b + [crc7(b) << 1 | 1]


def bits_of(b):
    return [(byte >> i) & 1 for byte in b for i in range(7, -1, -1)]


CLK, CMD, DAT0 = 2, 3, 4


class Bus:
    """The host, the card's state machines and the lines between them."""

    def __init__(self, period, sms):
        self.period, self.sms = period, sms
        self.t = 0
        self.host_cmd = None        # what the host drives on CMD, or None
        self.history = []
        self.cpu = []               # (when, action)

    def clk(self, t):
        return 1 if (t % self.period) >= self.period / 2 else 0

    def level(self, pin, t):
        if pin == CLK:
            return self.clk(t)
        for sm in self.sms:
            if sm.pin == pin and sm.out_dir:
                if pin == CMD and self.host_cmd is not None:
                    raise AssertionError("host and card both drive CMD at %d" % t)
                return sm.out_val
        if pin == CMD and self.host_cmd is not None:
            return self.host_cmd
        return 1                    # the pull-ups

    def tick(self):
        now = {p: self.level(p, self.t) for p in (CLK, CMD, DAT0)}
        self.history.append(now)
        seen = self.history[-1 - SYNC] if len(self.history) > SYNC else {CLK: 0, CMD: 1, DAT0: 1}
        for sm in self.sms:
            sm.step(seen)
        for item in list(self.cpu):
            if item[0] <= self.t:
                item[1]()
                self.cpu.remove(item)
        self.t += 1
        return now

    def edge(self, rising):
        """Run to the next edge of the clock; the levels just before it."""
        while True:
            before = {p: self.level(p, self.t) for p in (CLK, CMD, DAT0)}
            self.tick()
            if self.clk(self.t) != before[CLK] and self.clk(self.t) == rising:
                return before


def host_command(bus, frame):
    """The host sends a frame: each bit put on CMD after a falling edge.  Returns the
    number of the rising edge at which the end bit was read."""
    bits = bits_of(frame)
    rises = 0
    bus.edge(0)
    for b in bits:
        bus.host_cmd = b
        bus.edge(1)
        rises += 1
        bus.edge(0)
    bus.host_cmd = None
    return rises


def host_response(bus, nbits, limit=80):
    """Rising edges until a start bit on CMD (at most `limit`), then the bits."""
    for n in range(limit):
        if bus.edge(1)[CMD] == 0:
            break
    else:
        raise AssertionError("no response")
    bits = [0]
    while len(bits) < nbits:
        bits.append(bus.edge(1)[CMD])
    return n, bits


def frame_bytes(w0, w1):
    return [w0 >> 25, w0 >> 17 & 0xff, w0 >> 9 & 0xff, w0 >> 1 & 0xff, (w0 & 1) << 7 | w1 >> 25, w1 >> 17 & 0xff]


def pack(b):
    words = [8 * len(b) - 1]
    for i in range(0, len(b), 4):
        chunk = b[i:i + 4] + [0] * (4 - len(b[i:i + 4]))
        words.append(chunk[0] << 24 | chunk[1] << 16 | chunk[2] << 8 | chunk[3])
    return words


def test_command(programs, period, cpu_delay, response_bytes, lead=0):
    sm = SM(programs["sd_cmd"], CMD)
    bus = Bus(period, [sm])
    for _ in range(int(period * 8) + lead):
        bus.tick()
    frame = command(17, 0x00012345)

    def answer():
        assert len(sm.rx) == 2, sm.rx
        got = frame_bytes(sm.rx.pop(0), sm.rx.pop(0))
        assert got == frame, (got, frame)
        sm.tx.extend(pack(response_bytes))

    watch = {"armed": True}

    def poll():
        if watch["armed"] and len(sm.rx) == 2:
            watch["armed"] = False
            bus.cpu.append((bus.t + cpu_delay, answer))
        elif watch["armed"]:
            bus.cpu.append((bus.t + 1, poll))

    bus.cpu.append((0, poll))
    host_command(bus, frame)
    gap, bits = host_response(bus, 8 * len(response_bytes))
    assert bits == bits_of(response_bytes), "response bits"
    # Ncr: clocks between the command's end bit and the response's start bit.
    assert gap >= 2, "Ncr %d" % gap
    assert gap <= 64, "Ncr %d" % gap
    # The line let go after the end bit.
    for _ in range(3):
        bus.edge(1)
    assert sm.out_dir == 0, "CMD still driven"
    # And the next command is taken.
    watch["armed"] = True
    frame = command(13, 0x5d1b0000)
    response_bytes[:] = [13] + response_bytes[1:]
    bus.cpu.append((bus.t, poll))
    host_command(bus, frame)
    host_response(bus, 8 * len(response_bytes))
    return gap


def test_no_response(programs, period):
    sm = SM(programs["sd_cmd"], CMD)
    bus = Bus(period, [sm])
    for _ in range(int(period * 8)):
        bus.tick()

    def answer():
        sm.rx.clear()
        sm.tx.append(0)

    def poll():
        if len(sm.rx) == 2:
            bus.cpu.append((bus.t + 50, answer))
        else:
            bus.cpu.append((bus.t + 1, poll))
    bus.cpu.append((0, poll))
    host_command(bus, command(0, 0))
    for _ in range(70):
        assert bus.edge(1)[CMD] == 1, "a response to CMD0"
    assert sm.out_dir == 0
    bus.cpu.append((bus.t, poll))
    host_command(bus, command(0, 0))


def test_data(programs, period, blocks=2, lead=0):
    sm = SM(programs["sd_dat"], DAT0)
    bus = Bus(period, [sm])
    for _ in range(lead):
        bus.tick()
    want = []
    for k in range(blocks):
        data = [(0x9e3779b1 * (i + 1 + 128 * k)) & 0xffffffff for i in range(128)]
        crc = 0xbeef - k
        sm.tx.extend([4111] + data + [crc << 16])
        bits = [((w >> i) & 1) for w in data for i in range(31, -1, -1)]
        bits += [(crc >> i) & 1 for i in range(15, -1, -1)]
        want.append(bits)
    gaps = []
    for k in range(blocks):
        n = 0
        while bus.edge(1)[DAT0] == 1:
            n += 1
            assert n < 100, "no start bit"
        gaps.append(n)
        got = [bus.edge(1)[DAT0] for _ in range(4112)]
        assert got == want[k], "block %d: first difference at bit %d" % (k, next(i for i in range(4112) if got[i] != want[k][i]))
        assert bus.edge(1)[DAT0] == 1, "end bit"
    for _ in range(4):
        bus.edge(1)
    assert sm.out_dir == 0, "DAT0 still driven"
    assert gaps[1] >= 2, "blocks %d clocks apart" % gaps[1]
    return gaps


def main():
    programs, _ = load(sys.argv[1])
    r1 = [17, 0x00, 0x00, 0x09, 0x00]
    r1.append(crc7(r1) << 1 | 1)
    r2 = [0x3f] + list(range(1, 16)) + [0x55]
    failures = 0
    cases = [("264 kHz", 757.6), ("16.9 MHz (/4)", 200 / 16.93), ("22.6 MHz (/3)", 200 / 22.58), ("33.9 MHz (/2)", 200 / 33.87)]
    for name, period in cases:
        for what, run in [
            ("R1, the CPU at once", lambda: test_command(programs, period, 20, list(r1))),
            ("R1, the CPU 1.5 us late", lambda: test_command(programs, period, 300, list(r1))),
            ("R2 (136 bits)", lambda: test_command(programs, period, 100, list(r2))),
            ("no response", lambda: test_no_response(programs, period)),
            ("two data blocks", lambda: test_data(programs, period) if period < 100 else "skipped"),
        ]:
            try:
                result = run()
                print("ok    %-14s %-24s %s" % (name, what, "" if result is None else result))
            except AssertionError as e:
                failures += 1
                print("FAIL  %-14s %-24s %s" % (name, what, e))
    # The same at other phases of the host's clock against the state
    # machines' start.
    for name, period in cases[1:]:
        bad = []
        for lead in range(int(period) + 1):
            for what, run in [("R1", lambda: test_command(programs, period, 37 + 13 * lead, list(r1), lead)),
                              ("data", lambda: test_data(programs, period, 2, lead))]:
                try:
                    run()
                except AssertionError as e:
                    bad.append("%s at %d: %s" % (what, lead, e))
        failures += len(bad)
        print("%s  %-14s every phase                %s" % ("FAIL" if bad else "ok  ", name, "; ".join(bad)))
    print("piosim: %s" % ("%d FAILED" % failures if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
