# Project-local UDP NAPT correction

## Provenance

The installed Arduino-ESP32 3.2.0 library bundle is `idf-release_v5.4-2f7dcd86-v1`.
Its `versions.txt` and `CONFIG_ARDUINO_IDF_COMMIT` identify ESP-IDF commit
`2f7dcd862a`, and `esp_idf_version.h` identifies version 5.4.1.
The GitHub submodule API resolves its `components/lwip/lwip` to esp-lwip commit
`fa4dffdfac52587a26c1f29258376df750cf8474`:

- https://api.github.com/repos/espressif/esp-idf/contents/components/lwip/lwip?ref=2f7dcd862a
- https://github.com/espressif/esp-lwip/blob/fa4dffdfac52587a26c1f29258376df750cf8474/src/core/ipv4/ip4_napt.c

The byte-for-byte downloaded original is `ip4_napt-fa4dffdf.c.txt` (30,736 bytes):

- SHA-256: `526cf5d0fed387748c9699fb4b89b581d957122a50ac268f9195ac0e12553f0c`
- Git blob SHA-1: `c02b599ed79135dfde8c62bad92ddd6dcfb4bd2d`

The original license and notices are retained in the project-local source.

## Behavior and bounded change

The original outbound lookup keys UDP entries by protocol, client address, and
client source port, then overwrites the remote address/port on each send. Its
inbound lookup requires the remote address/port too. When one UDP socket sends
to two peers before their replies arrive, the last peer replaces the first and
the earlier peer's reply no longer matches.

`ESP32_S3/ip4_napt_udp_fix.c` contains the exact original plus a UDP-only
outbound lookup keyed by protocol and both address/port pairs. Every peer now
keeps a distinct mapping; the existing allocator checks active external ports
and allocates a different port when the original source port is already used.
Inbound address/port filtering, TCP and ICMP behavior, source-port restrictions,
512-entry table limit, garbage collection, and the 2-second UDP idle timeout
are unchanged. The patch does not open unsolicited inbound UDP access.

The full reproducible diff is `ip4_napt-udp-five-tuple.patch`. SDK files and
libraries in Arduino15 have not been edited. Arduino compiles the local C file
into an ordinary sketch object that must satisfy all public NAPT symbols before
the linker considers the stock archive member. Confirm this in each final map:
functions must resolve to `sketch/ip4_napt_udp_fix.c.o`, with no loaded
`liblwip.a(ip4_napt.c.obj)` member. The installed library exports eight NAPT/port
mapping functions; the complete source retains all public definitions, including
conditionally compiled maintenance/statistics APIs.

## SDK upgrade requirement

This replacement is tied to ESP-IDF 5.4.1 commit `2f7dcd862a` and its exact lwIP
submodule. Audit and rebase or remove it before any Arduino/ESP-IDF upgrade.
Version numbers alone cannot establish source or ABI equivalence. Retain the
original snapshot and compare the updated SDK source and headers, then repeat
shared-socket multi-peer UDP, ordinary UDP, DNS, HTTP/HTTPS, and link-map checks.
