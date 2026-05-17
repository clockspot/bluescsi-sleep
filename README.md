# bluescsi-sleep

A System 7 extension that recovers the [BlueSCSI](https://bluescsi.com) DaynaPORT (WiFi) network after the Mac wakes from sleep.

Tested on a PowerBook 180 running System 7.5.5 with Open Transport and a BlueSCSI v2 (Pico W) in WiFi-DaynaPORT mode. Should work on any 68k Mac with deep sleep and a BlueSCSI emulating a Dayna SCSI/Link device. Developed with Claude Code.

## The problem

When a PowerBook wakes from sleep, the DaynaPORT network does not come back. The `.ENET` driver still responds to status probes, and Open Transport itself reloads cleanly, but TCP/IP requests (`OTOpenInternetServices`) hang for ~90 seconds and then fail with `kEHOSTUNREACHErr` (-3259). Toggling the TCP/IP control panel or WiFi desk accessory don't help - only a reboot addresses it.

The cause is in the BlueSCSI firmware: after the PowerBook's deep sleep, BlueSCSI's per-target `scsiNetworkEnabled` flag and inbound packet queue end up in an inconsistent state, and the Mac-side driver doesn't know it needs to re-issue the enable command (it only sends it at driver-open time, which doesn't happen on wake).

## The fix

The extension installs a Sleep Queue callback. On every `sleepWakeUp`, a task-level Notification Manager callback sends Dayna SCSI/Link command `0x0E` (toggle interface, enable variant) directly to the BlueSCSI SCSI ID via the synchronous SCSI Manager. This bypasses the `.ENET` driver and Open Transport entirely; it speaks straight to BlueSCSI's firmware, which resets the inbound queue and re-asserts the enabled flag. Network connectivity returns immediately.

The BlueSCSI SCSI ID is discovered on the first wake by walking IDs 0–6 with `INQUIRY` (`0x12`) and matching "Dayna" or "SCSI/Link" in the vendor or product field. The result is cached for subsequent wakes.

For technical details, see [DETAILS.md](DETAILS.md).

## Install

Copy the extension to the System Folder's Extensions folder and restart.

For verbose diagnostics via a [log file](#log-entries-diagnostic-init-only), install the `-diag` variant.

Don't install both at the same time; they have the same `'INIT'` resource ID.

### Alert dialogs

If something goes wrong, the extension displays an alert dialog:

| Alert text | Meaning | What to do |
| --- | --- | --- |
| `BlueSCSI Sleep INIT: DaynaPORT not detected on the SCSI bus. INIT will retry on each wake.` | First-wake bus walk found no device matching `"Dayna"` or `"SCSI/Link"`. Either the BlueSCSI isn't powered up, or it isn't configured for DaynaPORT emulation. Sticky — shown once per session even if discovery keeps failing. | Check that the BlueSCSI is present on the SCSI bus and configured for WiFi-DaynaPORT in `bluescsi.ini`. |
| `BlueSCSI Sleep INIT: SCSI 0x0E enable failed (err=N stat=N). Network may not recover until restart.` | The SCSI command itself failed. `err` is the Mac SCSI Manager error; `stat` is the device-side status byte. Re-shown on each subsequent failed wake (rare). | Most often a transient bus contention issue. If it persists, install the diagnostic INIT for more context. |

### Log entries (diagnostic INIT only)

The diagnostic INIT appends to a text file `BlueSCSI Sleep Log` in the System Folder. Successful wakes produce concise lines; the first wake includes a one-time bus walk so you can see exactly what's on the SCSI bus. Example:

```
=== BlueSCSI Sleep INIT (diagnostic) active ===
=== Wake 1 ===
Walking SCSI bus:
  id=0 err=2 stat=0
  id=1 err=0 stat=0 v="QUANTUM " p="BlueSCSI Pico   "
  id=2 err=0 stat=0 v="QUANTUM " p="BlueSCSI Pico   "
  id=3 err=2 stat=0
  id=4 err=0 stat=0 v="Dayna   " p="SCSI/Link       " <- match
  id=5 err=2 stat=0
  id=6 err=2 stat=0
Selected SCSI ID=4
SCSI 0x0E enable id=4 err=0 stat=0
=== Wake 2 ===
SCSI 0x0E enable id=4 err=0 stat=0
```

Key things to read:

- **`id=N err=2 stat=0`** — selection timeout on that ID; no device is responding (normal for empty IDs).
- **`id=N err=0 stat=0 v="…" p="…"`** — device responded to INQUIRY. The vendor and product strings are space-padded ASCII.
- **`<- match`** — the discovery heuristic picked this target.
- **`Selected SCSI ID=N`** — the cached target for all subsequent wakes.
- **`SCSI 0x0E enable id=N err=E stat=S`** — wake-time SCSI command. `err=0 stat=0` is the only healthy combination; anything else triggers the SCSI-failed alert above.

## Build

Requires [Retro68](https://github.com/autc04/Retro68).

```
mkdir build && cd build
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=~/Git/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
make
```

Produces both variants by default:

- `BlueSCSISleepINIT.bin` — production INIT (install this).
- `BlueSCSISleepINIT-diag.bin` — diagnostic INIT (verbose log).

The diagnostic variant is built from the same `init.c` with `-DBLUESCSI_SLEEP_DIAGNOSTIC=1`.

## Repository contents

| File | Role |
| --- | --- |
| `init.c` | INIT source. Sleep Queue → Notification Manager → SCSI 0x0E enable. Single source for both variants; the `BLUESCSI_SLEEP_DIAGNOSTIC` compile define selects whether the log-writing machinery is compiled in. |
| `init.r` | Rez source for the production variant — packages `BlueSCSISleepINIT.flt` as an `'INIT'` 128. |
| `init-diag.r` | Rez source for the diagnostic variant — packages `BlueSCSISleepINIT-diag.flt` as an `'INIT'` 128. |
| `CMakeLists.txt` | Retro68 build for both variants. |
