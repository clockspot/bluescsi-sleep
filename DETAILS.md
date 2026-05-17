Supplement to [README.md](README.md).

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
    SleepQRec       sleepRec;             // The Sleep Queue record the OS holds a pointer to
    unsigned char   nmPending;            // 1 = wake NMRec queued (prevents double-install)
    unsigned char   errPending;           // 1 = error alert NMRec queued
    Boolean         discoveryAlertShown;  // sticky: only alert once per session
    unsigned char   _pad;
    short           wakeCount;
    short           scsiID;               // BlueSCSI SCSI ID; -1 = not yet discovered
    void           *copiedErrorAlertResp; // relocated ptr to ErrorAlertResp (set by _start)
    NMRec           nmRec;                // wake-reinit NMRec (re-queued on every wake)
    NMRec           errNmRec;             // error-alert NMRec (queued on failures)
    Str255          errString;            // Pascal-string buffer for the alert message
#if BLUESCSI_SLEEP_DIAGNOSTIC
    Boolean         logSpecValid;
    unsigned char   _pad2;
    FSSpec          logSpec;              // Log file location (lazy)
#endif
} ExtState;
```

The critical design trick: `sleepRec` is the **first field**. When the OS calls the Sleep Queue callback, it hands you a pointer to the `SleepQRec` record you installed — i.e. `qRecPtr` points to `state->sleepRec`. Because `sleepRec` is first, `qRecPtr` and `state` have the same numeric address. The callback can cast `qRecPtr` to `ExtState*` and reach any field without ever touching A5.

Note the two `NMRec` fields. The first is the wake-reinit record — re-queued on every `sleepWakeUp`. The second is the error-alert record — populated only when something goes wrong, with `nmStr` pointing at the `errString` buffer to make the Notification Manager pop a modal alert. The `copiedErrorAlertResp` field caches the relocated address of `ErrorAlertResp` so the alert-posting code (running inside the copied image) doesn't have to recompute it.

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

### Error reporting via the Notification Manager

When the INIT detects an error (DaynaPORT not found, or SCSI 0x0E command failed), it surfaces it to the user as a modal alert dialog. The mechanism is the same Notification Manager used for the wake-time bridge, but with the `nmStr` field of the `NMRec` populated:

```c
state->errNmRec.nmStr  = state->errString;   // Pascal string
state->errNmRec.nmResp = (NMUPP)state->copiedErrorAlertResp;
NMInstall(&state->errNmRec);
```

With `nmStr` non-NULL, the OS displays the string in a standard alert. The Apple menu blinks immediately and the alert pops the next time the user yields to the OS (e.g. on the next event-loop iteration of whatever app is frontmost). When the user dismisses the alert, the OS calls our `nmResp` callback (`ErrorAlertResp`), which calls `NMRemove` and clears the `errPending` flag so subsequent errors can re-alert.

The `errString` buffer is a `Str255` in the `ExtState` block — populated with a small Pascal-string formatting helper (`PStrSet`, `PStrAppendC`, `PStrAppendShort`) so the err and stat codes can be embedded in the message. The Notification Manager copies the string into its own queue, so we don't need to keep `errString` stable across the alert lifetime, but living in `ExtState` is simplest.

Two error sites in `ReinitViaNotification`:

- **Discovery failure** (BlueSCSI not detected). Sticky via `discoveryAlertShown` — only alerted once per session, even if discovery keeps failing on every wake. Without this guard, an unconfigured install would spam an alert on every sleep/wake cycle.
- **SCSI command failure** (`ScsiToggleInterface` returned non-zero err or stat). Re-shown on each subsequent failed wake, since this should be rare and the user wants to know each time.

### Build variants (production vs. diagnostic)

Both INIT binaries are built from a single `init.c`. The diagnostic build is compiled with `-DBLUESCSI_SLEEP_DIAGNOSTIC=1`, which:

- Enables the `LogOpen` / `LogClose` / `LogStr` / `LogShort` File Manager helpers, and the `logSpec` / `logSpecValid` fields in `ExtState`.
- Logs the bus walk on first wake, the selected SCSI ID, and a per-wake `SCSI 0x0E enable id=N err=E stat=S` line.

The production build leaves all of that out, both source and emitted code — the logging functions and `ExtState` log fields are inside `#if BLUESCSI_SLEEP_DIAGNOSTIC` guards. Both builds share the error-alert machinery: alerts are not a diagnostic feature, they're how the user finds out something is wrong.

CMake builds both by default via a small helper function (`add_bluescsi_init_variant`) that wires up the C target, the Rez packaging, and the right compile defines. Each variant has its own one-line `.r` file because Rez bakes the `.flt` filename into the binary at packaging time.

### Note on Retro68 INIT lifetime

The lifecycle above assumes the code resource — the `'INIT'` 128 binary that holds `SleepQGlue` and `ReinitViaNotification` — stays loaded in memory after `_start()` returns. On System 7.5.5, with Retro68's `--mac-flat` INIT layout, **it does not.** Even with `Get1Resource` + `DetachResource` + `HLock` + `HNoPurge`, the block is reclaimed during boot and the memory is reused (in our testing, by a `'sfnt'` font resource). The first time the OS dispatches `sleepWakeUp` it then jumps to garbage and crashes with an Illegal Instruction, often at a "DC.W ????" address inside whatever heap block now occupies that range. Retro68's [`libretro/relocate.c`](https://github.com/autc04/Retro68) carries a comment from the author warning that "all Retro68-compiled code resources have to be locked, or they might get moved as soon as the global variables are allocated below" — the standard 1995 INIT idiom isn't enough on top of Retro68's relocation layout.

The workaround (validated incrementally through a series of diagnostic INITs during development — see the development history below):

1. In `_start()`, allocate a fresh System-heap block sized to text + data (`&_edata - &_stext`).
2. `BlockMoveData` the relocated text + data out of the resource into the new block.
3. Walk the absolute relocation records that follow `_edata` in the resource and add `delta = newCode - origBase` to each kind-0/code and kind-1/data longword in the copy. Skip kind-2 (BSS — shared) and kind-3 (jump table — not used). Skip the relative pass entirely; PC-relative offsets within a verbatim copy are preserved.
4. Install copied addresses for any callback the OS will dispatch into later: `sleepQProc = &SleepQGlue + delta`, `nmRec.nmResp = &ReinitViaNotification + delta`.

The System-heap block survives boot independently of the resource map, so the OS dispatches into our copied code instead of into reclaimed memory.

### The lifecycle, end to end

1. **Boot** — the System runs `_start()`. The INIT copies its text+data into a System-heap block, walks Retro68's relocations to fix internal references in the copy, allocates an `ExtState` block in the System heap, fills in the Sleep Queue record and both `NMRec`s (wake-reinit and error-alert) with copied callback addresses, calls `SleepQInstall`, and returns.

2. **Normal use** — nothing happens. There is no overhead on every event loop iteration; the Sleep Queue callback only runs on sleep/wake events.

3. **Wake** — the OS walks the Sleep Queue and fires `sleepWakeUp` at interrupt level. The callback sets `nmPending = 1` and calls `NMInstall(&state->nmRec)`. Returns in microseconds.

4. **First task-level code after wake** — the Notification Manager fires `ReinitViaNotification`. On first wake it walks the SCSI bus and discovers the BlueSCSI ID. Then it issues the SCSI `0x0E` enable and calls `NMRemove`. Network is alive again. The user does not have to do anything.

5. **On error (rare)** — `ReinitViaNotification` calls `PostErrorAlert`, which populates `errString`, fills in `errNmRec`, and calls `NMInstall`. The OS pops a modal alert; on dismissal it calls `ErrorAlertResp`, which calls `NMRemove` and clears `errPending`.

## Development history

Initial discovery was done experimentally and incrementally via a standalone app (`main`) and a placeholder INIT (`init_minimal`), which can last be seen in commit `dfb0a97d69600847939eaac8a4e426a31cb70f9d`. A rough timeline of the milestones, in case any of it is useful to future contributors hitting similar walls:

1. **First attempt: a straightforward INIT** that called `KillIO` + `PBControlSync(csCode=0)` on the `.ENET` driver from a Sleep Queue callback at every wake. It crashed the machine the first time the user invoked sleep — Illegal Instruction at a "DC.W ????" address in the heap, courtesy of MacsBug.

2. **Diagnosing the crash** turned out to be the hard part. The `'INIT'` code resource doesn't survive `_start()` returning under Retro68's `--mac-flat` layout, even with the textbook `DetachResource` + `HLock` + `HNoPurge` incantation — the memory gets reused (in our testing, by a `'sfnt'` font resource) and the OS's first sleep-queue callback dispatches into garbage. This was bisected via a series of stepped-down minimal INITs (each adding one more behaviour from the suspected-crash path) until the lifetime issue was conclusively pinned on Retro68's flat-mac resource handling rather than anything wrong with the C code.

3. **Copy-and-relocate** as the fix. `_start()` allocates a System-heap block, `BlockMoveData`s the text+data out of the doomed resource, and walks Retro68's relocation records to re-point internal absolute references at the copy. Callbacks then get installed at the copied addresses. This was again validated step-wise — first a no-op interrupt-safe callback, then a Notification Manager round-trip, then File Manager I/O from the relocated code — before being trusted with anything that actually mattered.

4. **Driver-level reinit explored and ruled out.** With the INIT now surviving sleep, the original driver-level reinit was wired up. The driver responded happily (`KillIO=0`, `ctrl0=-17 controlErr` — `csCode=0` not implemented by this driver — `ctrl1=0 noErr`), and the network did absolutely not recover. Toggling TCP/IP in the control panel didn't help either; this was a useful data point that the wedge was not in OT's configured state.

5. **Open Transport layer probed.** A standalone app ran the TN1145 lazy-recreate dance — `InitOpenTransport`, then `OTOpenInternetServices`, then `OTInetStringToAddress`. `InitOpenTransport` returned `noErr` in 1 tick. `OTOpenInternetServices` returned `kEHOSTUNREACHErr` (-3259) after **5737 ticks (≈ 95 seconds)**. So OT itself reloads cleanly on wake, but the very next thing it tries — bringing up TCP/IP services — fails because there's no route to the host. That's the same `-3259` `iCab` and `Network Time` were already returning, just measured cleanly.

6. **The BlueSCSI firmware as the actual culprit.** With OT and driver both ruled out, the suspicion fell on the SCSI side. Reading [`lib/SCSI2SD/src/firmware/network.c`](https://github.com/BlueSCSI/BlueSCSI-v2/blob/main/lib/SCSI2SD/src/firmware/network.c) turned up Dayna SCSI/Link command `0x0E` ("toggle interface"), which on the BlueSCSI side does exactly two things: set `scsiNetworkEnabled` and clear the inbound packet queue. The Mac-side `.ENET` driver only sends this command at driver-open time, which doesn't happen on wake — and on wake, the BlueSCSI's per-target state is exactly what's broken.

7. **First implementation didn't work** — and that was its own diagnostic. Sleep / wake produced clean `err=0 stat=0` for the SCSI 0x0E command on both sides, but the network still hung. Reading the firmware source more carefully revealed that the enable/disable bit is in `cdb[5] & 0x80`, not in `cdb[4]` as the original Dayna spec implies. The INIT had been sending "disable" on both sleep and wake. Moving the bit to byte 5 fixed it; first sleep/wake after that recovered the network instantly.

8. **Production cleanup** dropped the driver-level reset code (since it was a confirmed no-op), the per-wake verbose logging, and the Unit Table driver lookup (`SCSIGet` serialises with any in-flight driver I/O on its own). The standalone OT-probe app and the minimal-INIT skeleton were retired once the production INIT was self-sufficient. Alert dialogs (via a second `NMRec` with `nmStr` populated) were added so the production build can stay silent on success but still surface failures to the user. The diagnostic build variant was added as a compile-time toggle so the verbose log path can be reintroduced without re-deploying a different INIT framework.

## Open questions / future work

- The INIT only re-enables the BlueSCSI's network state; it does not actively wake user-space TCP/IP services that already had providers open before sleep. Open Transport itself reloads cleanly when an app makes a fresh call after wake (per Apple TN1145), so apps following the standard "lazy reopen on `kOTProviderIsClosed`" pattern recover without intervention. Apps that don't may need to be relaunched.
- The SCSI Manager calls use the old (1986-era synchronous) API rather than SCSI Manager 4.3, for maximum compatibility across 68k Macs. Switching to 4.3 (`SCSIExecIO`) would allow async operation but isn't necessary for this short single-CDB command.
- The discovery heuristic could be tightened with a SCSI peripheral type check (`pdt == 0x03` AND vendor/product match) if the current heuristic ever produces a false positive on someone's bus.
