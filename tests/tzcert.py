#!/usr/bin/env python3
"""A test certificate with its dates given with a time zone, as some head unit
certificates have them (and RFC 5280 does not allow): for tests/tls_host.c.

    python3 -I sdbridge/tests/tzcert.py IN.crt OUT.crt

The validity's two times are rewritten as UTCTime with -0700 (the same instants);
nothing else is changed, so the signature no longer matches, which the test does
not need."""
import datetime
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('pem2c', os.path.join(here, '..', 'pem2c.py'))
pem2c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pem2c)


def zoned(tag, body):
    text = body.decode()
    t = datetime.datetime.strptime(text[:-1], '%y%m%d%H%M%S' if tag == 0x17 else '%Y%m%d%H%M%S')
    t -= datetime.timedelta(hours=7)
    return pem2c.element(0x17, (t.strftime('%y%m%d%H%M%S') + '-0700').encode())


def main():
    der = pem2c.pem_der(open(sys.argv[1]).read())
    tag, c, e = pem2c.tlv(der, 0)
    cert = pem2c.children(der, c, e)
    tbs_tag, tbs_c, tbs_e = pem2c.tlv(cert[0][1], 0)
    tbs = pem2c.children(cert[0][1], tbs_c, tbs_e)
    v = 4 if tbs[0][0] == 0xa0 else 3
    times = pem2c.children(tbs[v][2], 0, len(tbs[v][2]))
    validity = pem2c.element(0x30, b''.join(zoned(t, body) for t, _, body in times))
    tbs_der = pem2c.element(tbs_tag, b''.join(validity if i == v else raw for i, (_, raw, _) in enumerate(tbs)))
    out = pem2c.element(tag, tbs_der + b''.join(raw for _, raw, _ in cert[1:]))
    if pem2c.normalised(out) is None:
        sys.exit('tzcert: the dates did not come out with a time zone')
    open(sys.argv[2], 'w').write(pem2c.der_pem(out))


if __name__ == '__main__':
    main()
