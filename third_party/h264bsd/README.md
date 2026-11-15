# h264bsd, for the RNS-E SDK

An H.264 (baseline profile) decoder in portable C: the decoder that was
in the Android Open Source Project, as packaged by
https://github.com/oneam/h264bsd (commit 42bcb5d753ad86d84903354bf3c68423c28adb7b,
26 April 2022), the `src/` directory.  Licences in `LICENSE.md` (Apache
2.0 for the AOSP code, MIT for the packaging).

Changed for the SDK (each change is marked "RNS-E SDK" in the source):

- `h264bsd_reconstruct.c`: `h264bsdFillBlock` copies a block that lies
  inside the reference picture directly (`CopyBlock`), instead of a call
  of `memcpy` for each row; `h264bsdFastCopy = 0` gives the original.
- `h264bsd_decoder.c`, `.h`: an optional timer (`h264bsdTimer`) that adds
  up the loop filter's time in `h264bsdFilterTime`.

Built by `sdk/build.sh` for an example whose `libs` file names it, with
the C library headers of `sdk/third_party/libc`.  Used by
`sdk/examples/h264bench`; see `docs/android-auto.md`.
