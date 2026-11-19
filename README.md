# rnse-sdbridge

An SD card slot of the Audi RNS-E 192 (8P0 035 192) as a link into the
unit: firmware for an RP2040 that answers the 192 as an SD card made up
as it is read, and the 192's programs at the other end, on the way to
Android Auto on the unit's screen.

- `SDBRIDGE.UF2`: a read-only FAT16 card with `LINKTEST.BIN`, which
  `linkbench` (on the 192, from the SD loader of
  [rnse-sdk](https://github.com/jroark/rnse-sdk)) reads, checks and times;
- `SDPHONE.UF2`: the phone's side on a desk (USB host, Android Open
  Accessory, Android Auto's version exchange); `SDHANDSHAKE.UF2` adds the
  TLS handshake, built only with a certificate and key of your own;
- `SDLINK.UF2`: the bridge whole, the phone's video as `AALINK.BIN` on
  the card, which `aaviewer` (started by `aaview`) decodes and shows on
  the 192; `h264bench` times the decoder.

[docs/sdbridge.md](docs/sdbridge.md) has the wiring and what has been
tested, [docs/sdbridge-laptop.md](docs/sdbridge-laptop.md) the tools,
builds and flashing, [docs/android-auto.md](docs/android-auto.md) the
measurements and what is assumed. The card board is in
[rnse-hardware](https://github.com/jroark/rnse-hardware). The 192's
programs are tested on [rnse](https://github.com/jroark/rnse), the
emulator this was split out of with its history (at rnse commit
0e3a2d4ce5941161e17ff4816126281fbcf80088).

```sh
git clone https://github.com/jroark/rnse-sdk ../rnse-sdk
./build.sh                                  # out/SDBRIDGE.UF2 and the others (clones the pico-sdk, pinned)
sh tests/run.sh                             # the card logic on the host, the PIO programs in a simulator
sh ../rnse-sdk/build.sh 192 ./linkbench     # ../rnse-sdk/out/linkbench-192.bin
cargo test --release                        # linkbench and the viewer on the emulated 192
```

No firmware, keys or certificates are included, and none may be added:
Android Auto's head unit certificate and key stay outside every
repository. Your unit, your phone, your discs and your data are your own
risk. The 192's programs keep the display opening and the disc ejecting
and return to the SD loader when RETURN is held for two seconds: see
CLAUDE.md, "Mandatory hardware behavior".

MIT ([LICENSE](LICENSE)); `third_party/h264bsd` keeps its own licence,
and the pico-sdk and mbedTLS are downloaded, not included.
