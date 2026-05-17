# bluescsi-sleep

A System 7 extension that recovers the [BlueSCSI](https://bluescsi.com) DaynaPORT (WiFi) network after the Mac wakes from sleep — automatically, with no user action required.

Tested on a PowerBook 180 running System 7.5.5 with Open Transport and a BlueSCSI v2 (Pico W) in WiFi-DaynaPORT mode. Should work on any 68k Mac with deep sleep and a BlueSCSI emulating a Dayna SCSI/Link device. Developed with Claude Code.

## The problem

When a PowerBook wakes from sleep, the DaynaPORT network does not come back. The `.ENET` driver still responds to status probes, and Open Transport itself reloads cleanly, but the very first TCP/IP service call (`OTOpenInternetServices`, or anything an app like iCab does) hangs for ~90 seconds and then fails with `kEHOSTUNREACHErr` (-3259). The only manual recoveries are a warm reboot or reseating the BlueSCSI; toggling TCP/IP in the control panel does not help.

The cause is in the BlueSCSI firmware: after the PowerBook's deep sleep, BlueSCSI's per-target `scsiNetworkEnabled` flag and inbound packet queue end up in an inconsistent state, and the Mac-side driver doesn't know it needs to re-issue the enable command (it only sends it at driver-open time, which doesn't happen on wake).

## The fix

The extension installs a Sleep Queue callback. On every `sleepWakeUp`, a task-level Notification Manager callback sends Dayna SCSI/Link command `0x0E` (toggle interface, enable variant) directly to the BlueSCSI SCSI ID via the synchronous SCSI Manager. This bypasses the `.ENET` driver and Open Transport entirely — it speaks straight to BlueSCSI's firmware, which resets the inbound queue and re-asserts the enabled flag. Network connectivity returns immediately.

The BlueSCSI SCSI ID is discovered on the first wake by walking IDs 0–6 with `INQUIRY` (`0x12`) and matching "Dayna" or "SCSI/Link" in the vendor or product field. The result is cached for subsequent wakes.

## Install

1. Copy `build/BlueSCSISleepINIT.bin` to the System Folder's **Extensions** folder.
2. Restart.

After the first sleep/wake cycle, a one-time entry appears in `BlueSCSI Sleep Log` (in the System Folder) showing the SCSI bus walk and the selected target ID. Successful subsequent wakes are silent; only errors and undetected-bus cases get logged.

## Build

Requires [Retro68](https://github.com/autc04/Retro68).

```
mkdir build && cd build
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=~/Git/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
make
```

Produces:

- `BlueSCSISleepINIT.bin` — the production INIT (install this).
- `BlueSCSISleep.bin` — a standalone diagnostic app (see below).
- `BlueSCSISleepMinimalINIT.bin` — a no-op INIT skeleton (validated copy-and-relocate + NM dance with no driver work), kept around for future diagnostic work.

## Repository contents

| File | Role |
| --- | --- |
| `init.c` | Production INIT. Sleep Queue → Notification Manager → SCSI 0x0E enable. |
| `init.r` | Rez source — packages the flat code resource as an `'INIT'` 128. |
| `init_minimal.c` / `init_minimal.r` | Diagnostic skeleton — copy-and-relocate + NM round-trip with no driver work. Used during development; useful as a starting point for future on-wake experiments. |
| `main.c` | Standalone diagnostic app. Walks the SCSI bus, runs an Open Transport probe (the TN1145 lazy-recreate dance — `InitOpenTransport`, `OTOpenInternetServices`, `OTInetStringToAddress`), and logs every step's err code and elapsed ticks to `BlueSCSI Sleep Log`. Run it after wake to see exactly where OT is failing if the production INIT isn't doing the right thing on a different setup. |
| `CMakeLists.txt` | Retro68 build for all three targets. |

## How it works

### What the INIT does, conceptually

The DaynaPORT network appears broken after wake, but the BlueSCSI firmware is fine — it just needs a single SCSI command (`0x0E` enable) to reset its per-target network state. The Mac-side `.ENET` driver only sends that command at driver-open time, which doesn't happen on wake. So the INIT watches for wake events and sends the command itself.

### System Extensions (INITs)

On modern macOS you have login items, Launch Daemons, and kernel extensions. On System 7, the equivalent for "code that loads at boot and stays running in the background" is a System Extension, historically called an INIT (short for "initialise").

At startup, before the Finder appears, the System walks through the Extensions folder and runs every file it finds there. It does this by loading the file, finding a resource inside it of type `'INIT'`, and jumping to the code stored in that resource. Your `_start()` function is that entry point.

Critically, after `_start()` returns, there is no process running your code. Classic Mac OS is co-operatively multitasked and single-threaded; there is no background thread you can leave spinning. So an INIT cannot just have a loop that checks a flag. Instead, it has to hook into the OS itself so the OS calls your code back at the right moments.

### The Sleep Queue

System 7 provides a Sleep Queue — a linked list of records the OS consults whenever the PowerBook is about to sleep or just woke up. Any piece of code can add itself to this list by calling `SleepQInstall`, and from then on the OS will call a function pointer stored inside the record at each sleep/wake event.

The OS sends different **messages** to every registered callback:

| Message | When | Privilege level |
| --- |  --- |  --- |
| `sleepRequest` | Mac wants to sleep; you can veto | Task level (normal) |
| `sleepDemand` | Mac is sleeping no matter what | Task level |
| `sleepWakeUp` | Mac just woke up | Interrupt level |
| `sleepRevoke` | A previous sleep request was cancelled | Varies |

"Privilege level" matters a great deal; explained below. For our purposes, `sleepWakeUp` is the one we care about, and it fires at interrupt level.

The Sleep Queue record (`SleepQRec`) is a small struct. You fill in a `sleepQType` magic number (16) and a function pointer (`sleepQProc`), and the OS manages the rest. Our record is embedded inside the larger `ExtState` block allocated in the System heap, so it persists after `_start()` exits.

### Interrupt level vs. task level

This is the most important concept in the whole file.

Classic Mac OS runs on a 68030 CPU which supports **hardware interrupt levels** (0–7). When a hardware interrupt fires — a keypress, a mouse move, a timer tick, or a power-management event like a wake signal — the CPU pauses whatever it was doing, raises the interrupt level, and runs the interrupt service routine. While the interrupt level is elevated:

- **No Toolbox calls are safe.** The Toolbox is not re-entrant. If your interrupt fires while it's in the middle of drawing a window, and your interrupt routine calls another Toolbox function, you'll corrupt internal state and crash.
- **Memory allocation is not safe** for the same reason.
- **The SCSI Manager is not safe.**
- **Only atomic operations are safe** — setting a boolean flag, incrementing a counter, reading a timer.

"Task level" means interrupt level 0 — normal code, running inside an application's event loop, where you can safely call any Toolbox function.

`sleepWakeUp` fires at interrupt level. So in `SleepQCallbackImpl` all we do is post a Notification Manager request:

```c
if (message == sleepWakeUp && !state->nmPending) {
    state->nmPending = 1;
    NMInstall(&state->nmRec);
}
```

`NMInstall` is documented as interrupt-safe: it does no memory allocation and simply links the record into a queue. We then return, and the hardware interrupt finishes. The actual SCSI work — `SCSIGet`, `SCSISelect`, `SCSICmd`, `SCSIComplete` — is done by the Notification Manager callback at task level, where it's safe.

### The register-based calling convention and the asm glue

Modern CPUs pass function arguments on the stack or in specific registers according to a well-defined ABI. 68k Mac OS predates standard ABIs. Many OS callbacks use a **register-based convention**: the OS puts arguments in specific CPU registers (D0, A0, etc.) and jumps to your function. GCC (the compiler Retro68 uses) does not support this for parameters — it only generates stack-based function calls.

So the file has a hand-written assembly stub, `SleepQGlue`, that sits between the OS and the C function:

```asm
SleepQGlue:
    move.l %a0, -(%sp)       ; push qRecPtr (was in register A0) onto stack
    move.l %d0, -(%sp)       ; push message (was in register D0) onto stack
    jsr SleepQCallbackImpl   ; call the C function normally
    addq.l #8, %sp           ; clean up the two arguments we pushed
    rts
```

Think of it as a shim: it takes the OS's register-based calling convention and converts it to the stack-based convention that C expects.

### The A5 world problem

On classic Mac OS, each application has a private data segment in memory, and the CPU's A5 register always points into it. C global variables (anything declared at file scope) are stored relative to A5. When GCC generates code like `gMyFlag = 1`, it compiles to something like `move.b #1, offset(A5)` — the variable's location is relative to wherever A5 currently points.

When the OS calls your Sleep Queue callback, A5 holds the **current application's** A5 value, not yours. Your global variables are somewhere completely different. If you try to read or write your C globals inside a callback, you read garbage.

The solution is to **never use C globals in any callback**. All mutable state lives in a block of memory allocated with `NewPtrSysClear()` — a System heap allocation that does not depend on A5. You pass a pointer to that block through other means.

### The ExtState block

`NewPtrSysClear()` allocates memory from the **System heap** — a special region of memory that the OS owns and that persists for the entire life of the machine, regardless of which application is running or what A5 holds. It's the INIT equivalent of `malloc` with infinite lifetime, and it zeros the allocation for you.

```c
typedef struct ExtState {
    SleepQRec       sleepRec;       // The Sleep Queue record the OS holds a pointer to
    unsigned char   nmPending;      // 1 = notification already queued (prevents double-install)
    Boolean         logSpecValid;
    short           wakeCount;
    short           scsiID;         // BlueSCSI SCSI ID; -1 = not yet discovered
    short           _pad;
    FSSpec          logSpec;        // Log file location (lazy)
    NMRec           nmRec;          // Notification Manager record (re-queued on every wake)
} ExtState;
```

The critical design trick: `sleepRec` is the **first field**. When the OS calls the Sleep Queue callback, it hands you a pointer to the `SleepQRec` record you installed — i.e. `qRecPtr` points to `state->sleepRec`. Because `sleepRec` is first, `qRecPtr` and `state` have the same numeric address. The callback can cast `qRecPtr` to `ExtState*` and reach any field without ever touching A5.

### Task-level reinit via the Notification Manager

The SCSI Manager and File Manager are not interrupt-safe, but `sleepWakeUp` fires at interrupt level. The Notification Manager is the standard 68k bridge between these worlds: calling `NMInstall` from interrupt level asks the OS to run your `nmResp` callback the next time normal task-level code is executing.

The Notification Manager record (`NMRec`) is embedded directly in the `ExtState` block. Its `nmRefCon` field holds a pointer back to the `ExtState` so the callback can reach the SCSI ID and log file without A5:

```c
state->nmRec.nmResp   = ReinitViaNotification;  // task-level callback
state->nmRec.nmRefCon = (long)state;            // passed back to us on each call
state->nmRec.nmMark   = 0;                      // no Apple-menu mark
state->nmRec.nmIcon   = NULL;                   // no icon
state->nmRec.nmSound  = NULL;                   // no sound
state->nmRec.nmStr    = NULL;                   // no alert dialog
```

With all the alert fields set to NULL, the notification is completely silent — the user sees nothing. Its only effect is to schedule `ReinitViaNotification` to run at task level, where the SCSI Manager calls are safe.

### Direct SCSI Manager calls (bypassing the .ENET driver)

The SCSI command sequence is `SCSIGet` → `SCSISelect(id)` → `SCSICmd(cdb, 6)` → `SCSIComplete(&stat, &msg, timeout)`. The 6-byte CDB is `0E 00 00 00 00 80`. BlueSCSI's firmware (in [`lib/SCSI2SD/src/firmware/network.c`](https://github.com/BlueSCSI/BlueSCSI-v2/blob/main/lib/SCSI2SD/src/firmware/network.c)) checks `cdb[5] & 0x80`: bit set ⇒ `scsiNetworkEnabled = true` and inbound queue cleared; bit clear ⇒ `scsiNetworkEnabled = false`. (The original Dayna SCSI/Link spec places the bit in byte 4 — that's wrong for BlueSCSI, and was a real bug during development.)

`SCSIGet` serialises against any in-flight `.ENET` driver I/O, so no explicit `KillIO` of the driver is needed before our command. The interleaving is invisible to the driver — from its perspective, one of its own commands just took slightly longer to issue.

### SCSI ID auto-discovery

On the first wake, the INIT walks IDs 0–6 with `INQUIRY` (`0x12`), reads 36 bytes from each that responds, and matches `"DAYNA"` or `"SCSI/LINK"` in the vendor or product field. The first match is cached in `state->scsiID` and used for all subsequent wakes.

The match heuristic deliberately does **not** match on `"BLUESCSI"`: BlueSCSI HDD emulations on the same bus legitimately put that string in their product field, and matching on it would catch the wrong target. Real BlueSCSI DaynaPORT hardware reports `vendor="Dayna"` `product="SCSI/Link"` with SCSI peripheral type `0x03` (processor device) — *not* `0x09` (communications) as the old Dayna spec implies.

### Note on Retro68 INIT lifetime

The lifecycle above assumes the code resource — the `'INIT'` 128 binary that holds `SleepQGlue` and `ReinitViaNotification` — stays loaded in memory after `_start()` returns. On System 7.5.5, with Retro68's `--mac-flat` INIT layout, **it does not.** Even with `Get1Resource` + `DetachResource` + `HLock` + `HNoPurge`, the block is reclaimed during boot and the memory is reused (in our testing, by a `'sfnt'` font resource). The first time the OS dispatches `sleepWakeUp` it then jumps to garbage and crashes with an Illegal Instruction, often at a "DC.W ????" address inside whatever heap block now occupies that range. Retro68's [`libretro/relocate.c`](https://github.com/autc04/Retro68) carries a comment from the author warning that "all Retro68-compiled code resources have to be locked, or they might get moved as soon as the global variables are allocated below" — the standard 1995 INIT idiom isn't enough on top of Retro68's relocation layout.

The workaround (validated through diagnostic steps in `init_minimal.c`):

1. In `_start()`, allocate a fresh System-heap block sized to text + data (`&_edata - &_stext`).
2. `BlockMoveData` the relocated text + data out of the resource into the new block.
3. Walk the absolute relocation records that follow `_edata` in the resource and add `delta = newCode - origBase` to each kind-0/code and kind-1/data longword in the copy. Skip kind-2 (BSS — shared) and kind-3 (jump table — not used). Skip the relative pass entirely; PC-relative offsets within a verbatim copy are preserved.
4. Install copied addresses for any callback the OS will dispatch into later: `sleepQProc = &SleepQGlue + delta`, `nmRec.nmResp = &ReinitViaNotification + delta`.

The System-heap block survives boot independently of the resource map, so the OS dispatches into our copied code instead of into reclaimed memory.

### The lifecycle, end to end

1. **Boot** — the System runs `_start()`. The INIT copies its text+data into a System-heap block, walks Retro68's relocations to fix internal references in the copy, allocates an `ExtState` block in the System heap, fills in the Sleep Queue record and the `NMRec` (with the copied callback addresses), calls `SleepQInstall`, and returns.

2. **Normal use** — nothing happens. There is no overhead on every event loop iteration; the Sleep Queue callback only runs on sleep/wake events.

3. **Wake** — the OS walks the Sleep Queue and fires `sleepWakeUp` at interrupt level. The callback sets `nmPending = 1` and calls `NMInstall(&state->nmRec)`. Returns in microseconds.

4. **First task-level code after wake** — the Notification Manager fires `ReinitViaNotification`. On first wake it walks the SCSI bus and discovers the BlueSCSI ID. Then it issues the SCSI `0x0E` enable, logs only if something failed, and calls `NMRemove`. Network is alive again. The user does not have to do anything.

## Open questions / future work

- The INIT only re-enables the BlueSCSI's network state; it does not actively wake user-space TCP/IP services that already had providers open before sleep. Open Transport itself reloads cleanly when an app makes a fresh call after wake (per Apple TN1145), so apps following the standard "lazy reopen on `kOTProviderIsClosed`" pattern recover without intervention. Apps that don't may need to be relaunched.
- The SCSI Manager calls use the old (1986-era synchronous) API rather than SCSI Manager 4.3, for maximum compatibility across 68k Macs. Switching to 4.3 (`SCSIExecIO`) would allow async operation but isn't necessary for this short single-CDB command.
- The discovery heuristic could be tightened with a SCSI peripheral type check (`pdt == 0x03` AND vendor/product match) if the current heuristic ever produces a false positive on someone's bus.
