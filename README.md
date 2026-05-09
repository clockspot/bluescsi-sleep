# bluescsi-sleep

A Macintosh System 7 extension intended to fix the DaynaPORT SCSI/Link driver not recovering after sleep/wake. For use with [BlueSCSI](https://bluescsi.com)'s WiFi-via-DaynaPORT-emulation feature.

**Status:** work in progress. Tested on a PowerBook 180 running System 7.5.5 with Open Transport. The INIT loads, installs a Sleep Queue handler, and survives boot via a copy-and-relocate workaround for a Retro68-specific code-resource lifetime issue (see "Note on Retro68 INIT lifetime" below). Driver-level reinit calls (`KillIO` + `PBControlSync`) execute cleanly but do **not** by themselves restore Open Transport's network stack; the OT-layer reinit mechanism is still being investigated.

Developed with Claude Code.

## Repository contents

| File | Role |
| --- | --- |
| `init.c` / `init.r` | The full System Extension — Sleep Queue + Notification Manager + driver reinit. Pre-dates the copy-and-relocate fix and currently crashes immediately on sleep; pending port of the mechanism proven in `init_minimal.c`. |
| `init_minimal.c` / `init_minimal.r` | Diagnostic INIT used to bisect the crash. Currently at "step 9" — copy-and-relocate machinery plus a no-driver-work Notification Manager dance, validating the path forward. |
| `main.c` | Standalone reinit utility. Run after wake, finds the driver, calls `KillIO` + `PBControlSync(csCode=0/1)`, writes a log to `BlueSCSI Sleep Log` in the System Folder, quits silently. |
| `CMakeLists.txt` | Retro68 build for all three targets. |

## How it works

According to Claude Code:

### What the extension does, conceptually

The DaynaPORT network driver stops working after sleep because it was designed for a card plugged into a real NuBus slot. When the Mac wakes up, it does not automatically tell every driver "hey, reinitialise yourself." The BlueSCSI firmware can emulate the DaynaPORT hardware perfectly, but the driver still needs a nudge — specifically, a flush of its pending I/O queue and a reset command — before it will talk to the network again.

The extension's job is: watch for every wake event, and when one happens, send that nudge to the driver.

### System Extensions (INITs)

On modern macOS you have login items, Launch Daemons, and kernel extensions. On System 7, the equivalent for "code that loads at boot and stays running in the background" is a System Extension, historically called an INIT (short for "initialise").

At startup, before the Finder appears, the System walks through the Extensions folder and runs every file it finds there. It does this by loading the file, finding a resource inside it of type `'INIT'`, and jumping to the code stored in that resource. Your `_start()` function is that entry point.

Critically, after `_start()` returns, there is no process running your code. Classic Mac OS is co-operatively multitasked and single-threaded; there is no background thread you can leave spinning. So an INIT cannot just have a loop that checks a flag. Instead, it has to hook into the OS itself so the OS calls your code back at the right moments. That is exactly what the two hooks below do.

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

-   **No Toolbox calls are safe.** The Toolbox (the Mac's built-in GUI and OS library) is not re-entrant. If your interrupt fires while the Toolbox is in the middle of drawing a window, and your interrupt routine calls another Toolbox function, you will corrupt internal state and crash.
-   **Memory allocation is not safe** for the same reason.
-   **Only atomic operations are safe** — setting a boolean flag, incrementing a counter, reading a timer.

"Task level" means interrupt level 0 — normal code, running inside an application's event loop, where you can safely call any Toolbox function.

`sleepWakeUp` fires at interrupt level. So in `SleepQCallbackImpl` all we do is call `NMInstall` to post a Notification Manager request:

```
if (message == sleepWakeUp && !state->nmPending) {
    state->nmPending = 1;
    NMInstall(&state->nmRec);
}
```

`NMInstall` is documented as interrupt-safe: it does no memory allocation and simply links the record into a queue. We then return, and the hardware interrupt finishes. The actual driver reset — which calls `KillIO` and `PBControlSync`, both Toolbox calls — is handled by the Notification Manager callback at task level.

### The register-based calling convention and the asm glue

Here the story gets a little weird.

Modern CPUs pass function arguments on the stack or in specific registers according to a well-defined ABI. 68k Mac OS predates standard ABIs. Many OS callbacks use a **register-based convention**: the OS puts arguments in specific CPU registers (D0, A0, etc.) and jumps to your function. GCC (the compiler Retro68 uses) does not support this for parameters — it only generates stack-based function calls.

So the file has a hand-written assembly stub, `SleepQGlue`, that sits between the OS and the C function:

```
SleepQGlue:
    move.l %a0, -(%sp)       ; push qRecPtr (was in register A0) onto stack
    move.l %d0, -(%sp)       ; push message (was in register D0) onto stack
    jsr SleepQCallbackImpl   ; call the C function normally
    addq.l #8, %sp           ; clean up the two arguments we pushed
    rts
```

Think of it as a shim: it takes the OS's register-based calling convention and converts it to the stack-based convention that C expects. `SleepQCallbackImpl` is then a perfectly normal C function that takes two arguments.

### The A5 world problem

On classic Mac OS, each application has a private data segment in memory, and the CPU's A5 register always points into it. C global variables (anything declared at file scope) are stored relative to A5. When GCC generates code like `gMyFlag = 1`, it compiles to something like `move.b #1, offset(A5)` — the variable's location is relative to wherever A5 currently points.

When the OS calls your Sleep Queue callback, A5 holds the **current application's** A5 value, not yours. Your global variables are somewhere completely different. If you try to read or write your C globals inside a callback, you read garbage.

For a standalone application (like the app version in `main.c`) this is not a problem — the application's own A5 is always set before the event loop runs, and the sleep callback fires while the app is running. But for an INIT, `_start()` returns and the INIT code has no application context at all. After `_start()` exits, whenever the OS calls your callback, A5 belongs to whichever application happens to be running at that moment.

The solution is to **never use C globals in any callback**. All mutable state lives in a block of memory allocated with `NewPtrSys()` — a System heap allocation that does not depend on A5. You pass a pointer to that block through other means.

### The ExtState block

`NewPtrSys()` allocates memory from the **System heap** — a special region of memory that the OS owns and that persists for the entire life of the machine, regardless of which application is running or what A5 holds. It is the INIT equivalent of `malloc` with infinite lifetime.

```
typedef struct ExtState {
    SleepQRec       sleepRec;      // The Sleep Queue record the OS holds a pointer to
    short           driverRefNum;  // Which driver to reset
    unsigned char   nmPending;     // 1 = notification already queued (prevents double-install)
    unsigned char   _pad;          // Alignment byte
    short           wakeCount;     // Number of wakes so far (for logging)
    Boolean         logSpecValid;
    unsigned char   _pad2;
    FSSpec          logSpec;       // Log file location
    NMRec           nmRec;         // Notification Manager record (queued on every wake)
} ExtState;
```

The critical design trick: `sleepRec` is the **first field**. When the OS calls the Sleep Queue callback, it hands you a pointer to the `SleepQRec` record you installed — i.e. `qRecPtr` points to `state->sleepRec`. Because `sleepRec` is first, `qRecPtr` and `state` have the same numeric address. The callback can cast `qRecPtr` to `ExtState*` and reach any field without ever touching A5.

```
long SleepQCallbackImpl(long message, SleepQRecPtr qRecPtr)
{
    ExtState *state = (ExtState *)qRecPtr;   // safe: same address, no A5
    if (message == sleepWakeUp && !state->nmPending) {
        state->nmPending = 1;
        NMInstall(&state->nmRec);            // interrupt-safe; no allocation
    }
    return 0;
}
```

### How the reinit happens at task level: the Notification Manager

We need something to run at task level — normal code, not an interrupt — to do the actual driver reset after the wake. The Mac OS **Notification Manager** is designed for exactly this bridge.

Calling `NMInstall` from interrupt level asks the OS to run a callback (`nmResp`) the next time normal task-level code is executing — i.e., when the system is back to the application event loop. The OS handles all the mechanics of deferring the call safely. There is no polling, no overhead on every event, and no stored addresses that could become stale.

The Notification Manager record (`NMRec`) is embedded directly in the `ExtState` block. Its `nmRefCon` field holds a pointer back to the `ExtState` so the callback can reach the driver ref num and log file without A5:

```
state->nmRec.nmResp   = ReinitViaNotification;  // our task-level callback
state->nmRec.nmRefCon = (long)state;            // passed back to us on each call
state->nmRec.nmMark   = 0;                      // no Apple-menu mark
state->nmRec.nmIcon   = NULL;                   // no icon
state->nmRec.nmSound  = NULL;                   // no sound
state->nmRec.nmStr    = NULL;                   // no alert dialog
```

With all the alert fields set to NULL, the notification is completely silent — the user sees nothing. Its only effect is to schedule `ReinitViaNotification` to run at task level.

`ReinitViaNotification` is a normal C function (with the `pascal` calling convention attribute, matching what the Notification Manager expects on 68k). It receives the `NMRecPtr`, recovers the `ExtState` pointer from `nmRefCon`, calls `KillIO` and `PBControlSync` to reset the driver, and then calls `NMRemove` to dequeue and clean up the record so it is ready to be installed again on the next wake.

### The driver system and the Unit Table

The DaynaPORT driver is part of Mac OS's **device driver** architecture. Drivers in classic Mac OS are identified by name (like `.ENET`, `.ENET1`, etc.) and given a **reference number** — a small negative integer — when they are opened. You use the reference number in all subsequent calls to the driver.

To find the driver by name, the code walks the **Unit Table**: a low-memory array of pointers to **Driver Control Entries** (DCE), one per installed driver. Low-memory globals are fixed addresses that the OS uses for global state — addresses baked into the architecture, not in any heap. `0x011C` holds a pointer to the Unit Table array; `0x01D2` holds its count.

Each DCE contains the driver's flags and a pointer (or handle) to the **driver header**, which begins with the driver's name as a Pascal string. Walking the table and comparing names finds the right entry. The reference number is derived as `~index` (bitwise NOT of the array index).

`KillIO(refNum)` cancels any I/O operations the driver may have queued before sleep. `PBControlSync` with `csCode = 0` sends a control call to the driver — the equivalent of sending an ioctl in Unix. Code zero is a tentative "soft reset" code; the exact code to use would need to be verified by disassembling the DaynaPORT driver, which is one of the open questions in the project.

### The lifecycle, end to end

1.  **Boot** — the System runs `_start()`. The INIT allocates an `ExtState` block in the System heap, fills in the Sleep Queue record and the `NMRec`, calls `SleepQInstall`, and returns. `Retro68FreeGlobals()` releases the INIT's own temporary C globals, but the `ExtState` block remains. (The code-resource side of "remains permanently" is more involved than it sounds — see the next section.)

2.  **Normal use** — nothing happens. There is no overhead on every event loop iteration, because the Sleep Queue callback only runs on sleep/wake events.

3.  **Wake** — the OS walks the Sleep Queue and fires `sleepWakeUp` at interrupt level. The callback sets `nmPending = 1` and calls `NMInstall(&state->nmRec)`. This links the record into the OS notification queue and returns in microseconds.

4.  **First task-level code after wake** — the Notification Manager fires `ReinitViaNotification`. It clears `nmPending`, calls `KillIO` and `PBControlSync` to flush and reset the driver, logs the result, and calls `NMRemove` to dequeue the record. Network is alive again. The user does not have to do anything.

### Note on Retro68 INIT lifetime

The lifecycle above assumes the code resource — the `'INIT'` 128 binary that holds `SleepQGlue` and `ReinitViaNotification` — stays loaded in memory after `_start()` returns. On System 7.5.5, with Retro68's `--mac-flat` INIT layout, **it does not.** Even with `Get1Resource` + `DetachResource` + `HLock` + `HNoPurge`, the block is reclaimed during boot and the memory is reused (in our testing, by a `'sfnt'` font resource). The first time the OS dispatches `sleepWakeUp` it then jumps to garbage and crashes with an Illegal Instruction, often at a "DC.W ????" address inside whatever heap block now occupies that range. Retro68's [`libretro/relocate.c`](https://github.com/autc04/Retro68) carries a comment from the author warning that "all Retro68-compiled code resources have to be locked, or they might get moved as soon as the global variables are allocated below" — the standard 1995 INIT idiom isn't enough on top of Retro68's relocation layout.

The workaround used here (validated through diagnostic steps in `init_minimal.c`):

1.  In `_start()`, allocate a fresh System-heap block sized to text + data (`&_edata - &_stext`).
2.  `BlockMoveData` the relocated text + data out of the resource into the new block.
3.  Walk the absolute relocation records that follow `_edata` in the resource and add `delta = newCode - origBase` to each kind-0/code and kind-1/data longword in the copy. Skip kind-2 (BSS — shared) and kind-3 (jump table — not used). Skip the relative pass entirely; PC-relative offsets within a verbatim copy are preserved.
4.  Install copied addresses for any callback the OS will dispatch into later: `sleepQProc = &SleepQGlue + delta`, `nmRec.nmResp = &ReinitViaNotification + delta`.

The System-heap block survives boot independently of the resource map, so the OS dispatches into our copied code instead of into reclaimed memory. `init_minimal.c` step 8 validated the relocation mechanism with a no-op callback; step 9 validates the full Sleep Queue → Notification Manager dance with no driver work. Step 10 will port the driver-reinit logic into `init.c` using the same machinery.

### Open Transport vs. driver-level reinit

The standalone reinit app in `main.c` finds the `.ENET0` driver, calls `KillIO`, then `PBControlSync` with `csCode = 0` and `csCode = 1`. All three calls return cleanly (`KillIO=0`, `ctrl0=-17` controlErr — `csCode=0` not supported by this driver, expected — `ctrl1=0` noErr). Network does **not** recover. Toggling the TCP/IP control panel (switching to AppleTalk, saving, switching back to Alternate Ethernet, saving) also does not recover. iCab and SevenTTY both hang ~90s and return `-23009` / `-3259`.

This suggests the failure mode is at the Open Transport / DLPI layer rather than the .ENET driver itself: even when the driver responds to control calls, OT's STREAMS state still reflects the pre-sleep configuration and OT does not re-issue DLPI initialization primitives without an explicit interface-down event. Figuring out the right OT-level reinit mechanism is an open question.