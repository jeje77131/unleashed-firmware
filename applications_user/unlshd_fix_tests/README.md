# Fix Tests 095

A Flipper Zero app that makes three defects of the unlshd-095 review show on the device.
They do not show in normal use. Each test reports **BUG** on unpatched firmware and **OK** on
patched firmware, and adds a line to `apps_data/unlshd_fix_tests/results.txt` on the SD card,
prefixed with the firmware version.

The other two defects of the review show in normal use and need no app: the Sub-GHz receive
list sending a different signal than the one on screen (2), and renaming a saved signal whose
name has 64 characters or more (4).

| Test | Defect | Unpatched | Patched |
|---|---|---|---|
| 1 Keyboard 256 chars | 1: `text_input` keeps the text length in a `uint8_t` | The field looks empty, the letter lands at 0 | Letter lands at 256, the end |
| 3a GPS callback | 3: `gps_set_location_callback()` returns while the old callback still runs | Late calls > 0 | 0 late calls |
| 3b Network callback | 3: same for `network_set_event_callback()` | Late calls > 0 | 0 late calls |
| 5a realloc | 5: `realloc()` copies the new size out of the old block | Non-zero bytes past the old block, neighbour data | Nothing past the old block |
| 5b calloc | 5: `calloc()` does not check `count * size` for overflow | A 4 GiB request gets a 4-byte block | Stops on "out of memory", reboots |

## How each test works

**1.** The keyboard opens with 256 digits and the cursor at their end. The unpatched draw
callback stores `strlen()` in a `uint8_t`, which reads 256 as 0, and clamps the cursor to it:
the field shows only the cursor, and a typed letter is inserted at position 0. The same draw
also writes past its stack buffer (1 to 4 bytes; seen with AddressSanitizer, silent on the
device).

**3a, 3b.** The app opens a local RPC session (`rpc_session_open()`) and feeds it, every 4 ms,
the message the companion app sends: an empty `gps_location` (PB_Main field 87) or
`network_state_changed` (field 83). The session hands it to the service, which calls the
app's callback by the same path as a fix or an event from the phone. The callback stays in for
2 ms. Meanwhile the app subscribes and unsubscribes 200 times. A call counts as late when the
unsubscribing setter had already returned while it ran: from that point an app is entitled to
free the context, and the call would use freed memory. The context is never freed while the
session runs, so the test itself is safe on unpatched firmware.

Feeding is paced on purpose. With a call always running, the session worker would take the
patched service's lock back right after each call, and the unsubscribing setter, which waits
for that lock, would not get it until feeding stops.

Opening an RPC session takes over the GPS and network bridges. If the phone app is connected,
reconnect it after these tests.

**5a.** A 16-byte block, then another block behind it holding `NEIGHBOUR-BLOCK!` twice. The
first block is grown to 48 bytes. `malloc()` returns zeroed memory on this firmware, so any
non-zero byte past the first 16 was copied from outside the old block.

**5b.** `calloc(1073741825, 4)`: 4 GiB + 4 bytes, which wraps to 4 in 32-bit arithmetic. The
app asks before running it, because patched firmware stops in `calloc()` with "out of memory",
as it does for any allocation it cannot serve, and the device reboots. The app logs the start
of the test first: when it is the last line of the log, the firmware stopped in `calloc()`.

## Checked on a PC

The app's own test code was run on a PC against the original and the patched firmware code,
with the services and the allocator compiled unchanged:

- 3a and 3b, five runs each: unpatched about 90 late calls out of about 127, patched 0.
- 5a: unpatched 26 to 30 non-zero bytes and the neighbour's data, patched none.
- 5b, in a 32-bit build: unpatched calloc returns a block, patched stops on "out of memory".
- 1: unpatched cursor 0, the field drawn as `|`, letter at 0; patched cursor 256, letter at
  256. AddressSanitizer flags the stack overflow in the unpatched draw on this same screen.
- The two RPC frames decoded with the firmware's own protobuf definitions (nanopb,
  `PB_DECODE_DELIMITED`): contents 87 and 83, nothing left over.

## Build

On this branch the app is already in `applications_user/unlshd_fix_tests`: run
`./fbt fap_unlshd_fix_tests`, then copy `build/f7-firmware-C/.extapps/unlshd_fix_tests.fap` to
`apps/Tools` on the SD card. It uses only symbols that the 095 API (88.26) exports, so the same
`.fap` runs on unpatched and patched firmware.
