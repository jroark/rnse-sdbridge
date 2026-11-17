#!/usr/bin/env python3
"""The head unit certificate and key for sdhandshake.c as a C header, from PEM files
outside the repository (sdbridge/build.sh, with AA_CERT and AA_KEY):

    python3 sdbridge/pem2c.py CERT.pem KEY.pem OUT.h

The header holds the key: it is written under sdbridge/work/, which git ignores, and
must not be committed (docs/sdbridge.md)."""
import sys


def literal(name, text):
    lines = ['static const char %s[] =' % name]
    for line in text.splitlines():
        lines.append('    "%s\\n"' % line.replace('\\', '\\\\').replace('"', '\\"'))
    return '\n'.join(lines) + ';\n'


def main():
    cert = open(sys.argv[1]).read()
    key = open(sys.argv[2]).read()
    if '-----BEGIN CERTIFICATE-----' not in cert:
        sys.exit('%s: not a PEM certificate' % sys.argv[1])
    if 'PRIVATE KEY-----' not in key:
        sys.exit('%s: not a PEM private key' % sys.argv[2])
    with open(sys.argv[3], 'w') as out:
        out.write('/* Made by sdbridge/pem2c.py: holds a private key; never commit it. */\n')
        out.write(literal('hu_cert_pem', cert))
        out.write(literal('hu_key_pem', key))


if __name__ == '__main__':
    main()
