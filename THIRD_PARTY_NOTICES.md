# Third-party notices

The native Windows build uses the following third-party components:

- Microsoft MsQuic 2.5.9 — MIT License
- WinDivert 2.2.2 — GNU Lesser General Public License v3
- Mbed TLS 3.6.6 — Apache License 2.0 or GPLv2
- nghttp2 1.69.0 — MIT License
- ls-qpack — MIT License
- PQClean ML-KEM-768 (portable clean implementation) — CC0 / public domain;
  pinned source and upstream notices in `native/vendor/pqclean/UPSTREAM.md`
- Manrope — SIL Open Font License 1.1

The distribution includes the applicable license files next to the executable
where required. Build dependencies are downloaded from their official release
locations and verified with pinned SHA-256 checksums.
