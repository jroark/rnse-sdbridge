#!/usr/bin/env python3
"""The head unit certificate and key for sdhandshake.c as a C header, from PEM files
outside the repository (sdbridge/build.sh, with AA_CERT and AA_KEY):

    python3 sdbridge/pem2c.py CERT.pem KEY.pem OUT.h

The header holds the key: it is written under sdbridge/work/, which git ignores, and
must not be committed (docs/sdbridge.md).

A certificate whose validity dates carry a time zone offset (a UTCTime such as
140704000000-0700, which RFC 5280 does not allow but which phones accept) is refused
by mbedTLS's parser.  For one of those the header has two forms of it: hu_cert_pem, a
copy with the dates written in UTC ("...Z") and the lengths around them mended, which
mbedTLS parses (its signature no longer matches, and nothing checks it: the head unit
does not verify its own certificate), and hu_cert_wire, the certificate as it was, which
is what goes to the phone (src/aa_tls.c).  HU_CERT_WIRE says whether it is there."""
import base64
import datetime
import re
import sys


def pem_der(text):
    body = re.search(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', text, re.S)
    if not body:
        return None
    return base64.b64decode(''.join(body.group(1).split()))


def der_pem(der):
    b = base64.b64encode(der).decode()
    return ('-----BEGIN CERTIFICATE-----\n' + '\n'.join(b[i:i + 64] for i in range(0, len(b), 64))
            + '\n-----END CERTIFICATE-----\n')


def tlv(der, at):
    """The element at `at`: (tag, start of its contents, end)."""
    tag = der[at]
    n = der[at + 1]
    at += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(der[at:at + k], 'big')
        at += k
    return tag, at, at + n


def length(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def element(tag, contents):
    return bytes([tag]) + length(len(contents)) + contents


def children(der, start, end):
    out = []
    while start < end:
        tag, c, e = tlv(der, start)
        out.append((tag, der[start:e], der[c:e]))
        start = e
    return out


def utc(tag, text):
    """A time as UTC with a Z, or None when it is that already."""
    m = re.fullmatch(r'(\d{10})(\d{2})?([+-]\d{4})', text) if tag == 0x17 else \
        re.fullmatch(r'(\d{12})(\d{2})?([+-]\d{4})', text)
    if not m:
        return None
    digits, sec, off = m.group(1), m.group(2) or '00', m.group(3)
    fmt = '%y%m%d%H%M%S' if tag == 0x17 else '%Y%m%d%H%M%S'
    t = datetime.datetime.strptime(digits + sec, fmt)
    sign = 1 if off[0] == '+' else -1
    t -= sign * datetime.timedelta(hours=int(off[1:3]), minutes=int(off[3:5]))
    if tag == 0x17 and not 1950 <= t.year <= 2049:
        return element(0x18, (t.strftime('%Y%m%d%H%M%S') + 'Z').encode())
    return element(tag, (t.strftime(fmt) + 'Z').encode())


def normalised(der):
    """The certificate with its validity in UTC, or None when it needs no change."""
    tag, c, e = tlv(der, 0)
    cert = children(der, c, e)
    tbs_tag, tbs_c, tbs_e = tlv(cert[0][1], 0)
    tbs = children(cert[0][1], tbs_c, tbs_e)
    # [0] version (when there), serial, signature algorithm, issuer, validity.
    v = 4 if tbs[0][0] == 0xa0 else 3
    times = children(tbs[v][2], 0, len(tbs[v][2]))
    fixed = [utc(t, body.decode('ascii', 'replace')) for t, _, body in times]
    if not any(fixed):
        return None
    validity = element(0x30, b''.join(f or raw for f, (_, raw, _) in zip(fixed, times)))
    tbs_der = element(tbs_tag, b''.join(validity if i == v else raw for i, (_, raw, _) in enumerate(tbs)))
    return element(tag, tbs_der + b''.join(raw for _, raw, _ in cert[1:]))


def literal(name, text):
    lines = ['static const char %s[] =' % name]
    for line in text.splitlines():
        lines.append('    "%s\\n"' % line.replace('\\', '\\\\').replace('"', '\\"'))
    return '\n'.join(lines) + ';\n'


def octets(name, data):
    lines = ['static const unsigned char %s[] = {' % name]
    for i in range(0, len(data), 16):
        lines.append('    ' + ' '.join('0x%02x,' % b for b in data[i:i + 16]))
    return '\n'.join(lines) + '\n};\n'


def main():
    cert = open(sys.argv[1]).read()
    key = open(sys.argv[2]).read()
    der = pem_der(cert)
    if der is None:
        sys.exit('%s: not a PEM certificate' % sys.argv[1])
    if 'PRIVATE KEY-----' not in key:
        sys.exit('%s: not a PEM private key' % sys.argv[2])
    fixed = normalised(der)
    with open(sys.argv[3], 'w') as out:
        out.write('/* Made by sdbridge/pem2c.py: holds a private key; never commit it. */\n')
        if fixed:
            out.write('/* The certificate\'s dates have a time zone: hu_cert_pem is a copy in UTC\n'
                      ' * for mbedTLS, hu_cert_wire the certificate as it is, for the phone. */\n')
            out.write(literal('hu_cert_pem', der_pem(fixed)))
            out.write('#define HU_CERT_WIRE 1\n')
            out.write(octets('hu_cert_wire', der))
        else:
            out.write(literal('hu_cert_pem', cert))
            out.write('#define HU_CERT_WIRE 0\n')
            out.write('static const unsigned char hu_cert_wire[1];\n')
        out.write(literal('hu_key_pem', key))
    if fixed:
        print('pem2c: the certificate\'s dates have a time zone; mbedTLS gets a copy in UTC, '
              'the phone the certificate as it is')


if __name__ == '__main__':
    main()
