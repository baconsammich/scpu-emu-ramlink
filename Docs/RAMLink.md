# CMD RAMLink

A cartridge-port box with battery-backed RAM, its own DOS in a 64K EPROM, a
real-time clock, a pass-through "RAM-Port" for another cartridge, and a parallel
bus to a CMD hard drive. To the C64 it is a disk device that happens to be made
of RAM, and it is fast enough that software loads effectively instantly.

Implementation: `Source/RAMLink/`. Tests: `Tests/RAMLink/test_ramlink.cpp`.

## What CMD's own documentation says

Two primary sources, both now checked, and they agree with each other and with
what the emulator measures.

**The SuperCPU 128 V2 User's Guide, "Startup Error Messages"** lists what the
accelerator checks at power-on:

| Code | Meaning |
|---|---|
| `#01` | Static RAM failure (Byte Addressing) |
| `#02` | Static RAM failure (Page Addressing) |
| `#03` | Static RAM failure (Zero Page/Stack) |
| `#04` | Kernal/BASIC ROM Checksum Error |
| `#05` | **RL-DOS ROM Checksum Error** |

`#05` is the one that matters here: **RL-DOS is part of the SuperCPU's own ROM
and is checksummed at startup.** The list stops at `#05`, so the `06` this
project has seen is not in it -- consistent with CMD's FAQ attributing error 06
to an inadequate power supply (see ROMs/README.md).

**The compendium's mirror table** names the same thing in the bank-1 map:

```
$E000-$FFFF  KERNAL       read only  CPU H/W registers disabled and KERNAL ROM enabled
$8000-$9FFF  RLDOS        read only  CPU DOS extensions enabled
$6000-$7FFF  ALT. KERNAL
```

So RL-DOS lives in bank-1 SRAM at `$8000-$9FFF` and is exposed when CPU DOS
extensions are enabled -- the window `dosExtensionMapsBank1()` implements, and
the one the KERNAL reaches by `JSR $D210` (a trampoline CMD's DOS writes into
its own `$D200` scratch: `STA $D07E` / `STA $D0BE` / `STA $D07F`) followed by
`JSR $9F21`.

**Hardware Compatibility, on RAMLink:** *"the SuperCPU enhances the performance
of RAMLink itself... all of the RAMLink switches still operate just as they
normally would without a SuperCPU."* RAMLink attaches to the SuperCPU's
**external cartridge port**, so the chain is C64 -> SuperCPU -> RAMLink.

**And, immediately above it, on ultimax cartridges:** *"Cartridges that use the
'Ultimax' memory map will not work with the SuperCPU... Due to the way these
cartridges replace the Kernal, they would render the SuperCPU inoperable."*

That is the documentary half of a result this emulator measured independently:
substituting RAMLink's KERNAL unconditionally stops CMD's DOS with
`SUPERCPU INITIALIZATION ERROR: 06`. RAMLink is listed as *compatible*, so on an
accelerated machine it must not be permanently replacing the KERNAL. Measurement
and manual agree, which is why `m_CartKernalWhenOff` defaults false.

## Source of truth

VICE's `vice/src/c64/cart/ramlink.c` and `vice/src/core/rtc/rtc-72421.c`, which
is the same methodology the SuperCPU register file was written against — see the
note at the top of `Source/SuperCPU/registers.h`. The published documentation for
RAMLink's register block is thin and partly wrong; VICE's implementation is
driven by CMD's own ROM and is the only description that has been run against
real software for any length of time.

## Register map

| Address | Direction | Meaning |
|---|---|---|
| `$DE00-$DEFF` | r/w | The window. What it looks at is selected at `$DFC0-$DFC3`. |
| `$DF20` | w | Disarm the RAM-Port REU trap |
| `$DF21` | w | Forward to the REU's command register, if armed |
| `$DF22` | w | Arm the RAM-Port REU trap |
| `$DF40-$DF43` | r/w | i8255A PPI — the CMD parallel bus, and the DOS ROM bank on port C bits 1-0 |
| `$DF60` | w | Map the RAMLink DOS ROM |
| `$DF70` | w | Unmap it |
| `$DF7E` | w | **RAMLink on** |
| `$DF7F` | w | **RAMLink off** |
| `$DF80-$DF9F` | w | Select which 256-byte page of the internal 8K appears at `$DE00`. The page is the low five bits of the **address**, not the value. |
| `$DFA0-$DFA2` | w | RAMCard address latch, bits 8-15, 16-23, 24-31 |
| `$DFB0-$DFBF` | r/w | RTC 72421 |
| `$DFC0-$DFC3` | w | Window source: 0 internal RAM, 1 RAMCard, 2 RAM-Port, 3 pass-through |

`$DF7E` and `$DF7F` **stay decoded when everything else is switched off**. They
have to: they are the only way back on. Everything else — including the window
at `$DE00` — declines while the unit is off, so the machine answers instead.

## What is implemented

The register file, both memory windows, the RAMCard size decode, the clock, and
the REU trap. All tested.

**The RAMCard size decode is the interesting part**, because it is how software
discovers how much is fitted, and getting it wrong reports the wrong size rather
than failing:

- At **4 MB or less**, the fitted RAM repeats every 4 MB across the whole 64 MB
  card address space. A *full* 4 MB card therefore has **no open bus anywhere**
  — every granule answers. A 2 MB card reads open at granules 2 and 3, and those
  holes repeat.
- Between **5 and 16 MB**, unfitted granules below 16 read open bus, and the
  16 MB pattern repeats to 64 MB. Eight megabytes is the smallest size with a
  genuine hole.
- A card built from a 4 MB SIMM plus a 1 MB one shows the odd megabyte repeated
  across the rest of its group rather than reading open.

## What is deliberately not implemented

Each of these is a decision with a reason, not an unfinished edge.

**The DOS ROM is not banked into `$8000-$BFFF`.** VICE's `ramlink_roml_read()`
and `ramlink_a000_bfff_read()` both open with *"do not map this for super cpu"*
and return `CART_READ_THROUGH` when the machine is an SCPU64. Those two
exclusions are real.

> **Correction.** An earlier version of this file generalised from those two
> exclusions to a third — `ramlink_romh_read()`, which has **no** such exclusion
> — and claimed "the SuperCPU's own DOS carries the RAMLink support", citing
> strings found in `scpu.rom`. That was wrong twice over. `scpu.rom[$A000..$BFFF]`
> is a byte-identical copy of the C64 BASIC ROM, so the `DEVICE NOT PRESENT`
> found there is the stock BASIC error message, and the 26.8% of code shared
> with `ramlink201.bin` sits at matching offsets in the JiffyDOS banks — both
> products ship JiffyDOS 6.01. The RAMLink DOS is in the RAMLink's own ROM, and
> a device emulated without it can answer a probe and never be opened.

**`$E000-$FFFF` IS served from the cartridge.** `ramlink_romh_read()` is not
excluded for SCPU64, and `scpu64mem.c`'s `scpu64_romh_read()` does call through
to it — RAMLink puts the machine in ultimax (`cart_config_changed_slot0(CMODE_RAM,
CMODE_ULTIMAX, CMODE_READ)`) and then decides per access what to pass through.

The image carries two C64 KERNALs, confirmed by inspection of a 2.01 dump:

| Offset | Image | Similarity to stock KERNAL |
|---|---|---|
| `$8000-$9FFF` | served while RAMLink is **on** | 5.6% — a different 8K, correct vectors |
| `$A000-$BFFF` | served while it is **off but fitted** | 97.4% — stock plus JiffyDOS patches |
| `$C000-$FFFF` | the C128 pair | vectors `$FF3D`/`$FF05`/`$FF17` |

`$FF00-$FF0F` and `$FFF0-$FFFF` are holes that keep coming from the normal map.
They are load-bearing: the bank-switch code and the CPU vectors live there and
have to stay put across the switch, or the machine could not switch back.

**One measured deviation.** Substituting while RAMLink is switched **off** stops
CMD's DOS 2.04 with `SUPERCPU INITIALIZATION ERROR: 06`. That was reproduced on
the host with `Tools/viceconf/scpu_trace` before it ever reached hardware, and it
is why `m_CartKernalWhenOff` defaults false on an accelerated machine. The
unaccelerated rule stays implemented and tested, because it is what VICE actually
does and what a non-SuperCPU build would need.

**Device 16 does not work yet, and this is why.** `scpu_trace --open16` drives
the KERNAL's own `SETLFS`/`SETNAM`/`OPEN`/`CHKOUT` at device 16 and still ends up
in the serial routines at `$EB18` — nothing intercepts, because with the
substitution gated on the ON state no wedge is ever installed. What is unresolved
is how the wedge reaches an accelerated machine at all, given that VICE excludes
the `$8000`/`$A000` mappings for SCPU64 while CMD's own DOS carries a RAMLink
driver reached through the `$D0BE`/`$D0BF` DOS-extension window. That is the next
thing to settle, and it needs either deeper work on CMD's ROM or a reference
machine.

**The CMD parallel bus has no device on the far end.** The i8255A's port latches
behave correctly and the DOS ROM's bank-select lines on port C work, so a probe
for a CMD HD fails cleanly and quickly. That is the honest answer when no drive
is attached, and attaching one would mean emulating a CMD HD.

**RAM-Port modes 2 and 3 decline rather than forwarding.** The REU in this
project sits on the machine's own I/O, not nested inside RAMLink's pass-through,
so there is nothing to forward to. The `$DF21` trap *is* wired to the REU,
because that is the path RAMLink's DOS uses to drive an REU whose I/O it has
claimed.

**The RTC does not let guest software set the time.** Writes are accepted and
read back — register 14 in particular, which is how RAMLink detects the clock
exists — but they do not move the host's clock. A device whose time could be
wound backwards by guest software makes file timestamps worse, not better.

## The clock, and why it says 1980

A Raspberry Pi in a C64 has no battery-backed RTC and no network. It does not
know the time. The default `CRAMLinkFixedClock` reports a fixed, obviously
placeholder instant rather than dressing an uptime counter up as a date: a file
written at "1 Jan 1980 00:00" is recognisably undated, while one written at
"1 Jan 1970 00:04" looks like real metadata and is not. Anything that does know
the time can supply it by implementing `IRAMLinkClock`.

Two details of the chip are easy to get backwards and both are pinned by tests:
it powers up in **12-hour** mode, and control bit 2 selects **24-hour** mode when
**set**. Register 15 is also asymmetric — a write takes the mode from bit 2 and
stop from bit 1, but a read reports them in bits 1 and 0. That is what VICE
does, and it is reproduced rather than tidied.

## The gate, and that it is not version-specific

CMD's KERNAL does not dispatch device accesses to RAMLink unconditionally. It
tests flags in the ACCELERATOR's own `$D200` scratch RAM first:

```
$FA52: JSR $FFA8      CIOUT
$FA57: BIT $D201
$FA5A: BVS $FA6E
$FA5C: BIT $D204
$FA5F: BPL $FA4E      <- taken while $D204 bit 7 is clear
$FA68: STA $DF7E      <- only reached past both gates
```

Forcing those flags (`scpu_trace --rl-flags`, a diagnostic) makes the dispatch
fire for the first time — `PC $00EC04 W $DF7E`, CMD's own KERNAL switching
RAMLink on. So the gate is confirmed rather than inferred, and what is missing
is whatever detection sets the flags: the accelerator's boot makes exactly one
cartridge access in a whole run.

**DOS 1.4 gates it the same way**, so this is a design constant and not a 2.04
quirk. Comparing the two images:

| | 2.04 | 1.4 |
|---|---|---|
| populated slices | `$00000-$1FFFF` | `$00000-$0FFFF` only |
| `$D201-$D204` BIT tests | 65 | 32 |
| RL-DOS slice | `$08000` | `$08000` |
| `STA $DF7E` sites | 62 | 31 |

1.4 is the SuperCPU 64 image; 2.04 duplicates the DOS in the upper 64K for the
C128, which accounts for roughly the doubling.

> **1.4 does not boot in this emulator**, and has not for as long as anyone has
> checked: it stops with `SUPERCPU INITIALIZATION ERROR: 02-0`, which the 128 V2
> manual defines as *Static RAM failure (Page Addressing)* — the accelerator's
> own SRAM test failing. Verified as pre-existing, not caused by the RL-DOS
> seeding: identical with and without it. 2.04 passes the same test, which is
> part of why `make sdcard` stages 2.04. Worth a look on its own, since a failing
> SRAM test is an emulation gap rather than a ROM preference.

## How a RAMCard is organised

From the RAMLink User's Manual, Section 2 "Partitioning & Configuration":

> RAMLink performs an **auto-configuration when powered up for the first time,
> or anytime it is powered up after power has been removed**... It then sets
> aside a small amount of RAM **at the very top of memory, stores a
> configuration table**, creates one or two partitions and then formats those
> partitions.
>
> A. **There must be at least one partition present on RAMLink in order for it
> to be usable as a RAM disk.**

And Section 3: the device number *"is **not controlled by hardware**, but is
instead kept in a table located in the system partition"*, preset to **16**.

Two things follow, and both matter for emulation:

* **A blank RAMCard is not a working device.** No partition, no RAM disk, and a
  `?DEVICE NOT PRESENT` is then correct behaviour rather than a bug.
* **The configuration is data on the card, not state in the hardware.** So a
  card image either carries it or it does not, and no amount of register
  emulation substitutes.

The layout, confirmed by inspecting a configured 8MB image (`ramlink.rl`):

```
$7FF5E0  ff 10 01 01 10 ...        device number $10 = 16
$7FF5F0  "RAMLINK     "            disk name, $AA padded
$7FF800  01 01 ff 00 00 "SYSTEM"   32-byte partition entries, CBM $A0-padded
$7FF820  00 00 01 00 00 "C64OS"    type $01 native
$7FF840  00 00 02 00 00 "1541-1"   type $02
$7FF880  00 00 04 00 00 "1581-1"   type $04
```

## Persistence — how the RAMCard gets its contents

The RAMCard is battery-backed on real hardware. A Pi has no battery, so without
help a card the user partitioned comes up blank on every cold start, which makes
the device close to useless.

**This fork loads a RAMCard image at startup.** Put a raw dump of the card's RAM
at `SCPU/ramlink.img` and it is read into the card before the machine starts.
`make sdcard` stages one from `ROMs/ramlink.rl` if present.

- **The image must MATCH the fitted card, not merely fit inside it.** An
  earlier version of this document said a smaller image "fills the front of the
  card — where the partition table lives". That is wrong, and the error matters:
  the system partition lives at the **TOP** of the card, at `size - $1000`, as
  the `$7FF5E0` addresses above show for an 8 MB card. Pad an 8 MB image out to
  16 MB and that structure lands in the middle, where RAMLink does not look.

  Measured, with CMD's own DOS booting against each:

  | card | image | result |
  |---|---|---|
  | 8 MB | 8 MB | system partition found, 0 bytes changed |
  | 16 MB | 8 MB padded | nothing found, 333 bytes changed |
  | 16 MB | via `mkramcard.py` | system partition found, 0 bytes changed |

  `Tools/mkramcard.py <in> <out> <MB>` relocates the system partition and
  rewrites the SYSTEM entry's start address. `make sdcard` picks the image to
  stage from `RAMLINK_SIZE` and warns if its size does not match.

  An image LARGER than the card is refused outright rather than truncated,
  because a silently half-loaded image is worse than a clear error.

- **It is written back only on request, and only on the button press.** Set
  `RAMLINK_PERSIST 1` and the card is saved to `SCPU/ramlink.img` when the
  button reboots the Pi. Off by default: a save replaces an image that may have
  taken real effort to build.

  This used to say saving was structurally impossible — that `boot.cpp`
  releases the FAT driver before `/DMA` is asserted and never brings it back.
  That was wrong too. `busdiag-e18.txt` has always been written *after*
  takeover, with `/DMA` held and IRQs temporarily re-enabled through
  `CScopedLoggingIRQs`; the machinery was already there. The save happens after
  `radBus.release()`, which is the one moment it is both safe and free: the
  emulated machine is stopped so the card cannot change under the write, the
  C64 has its own CPU back so slow SD I/O cannot starve the bus, and the Pi is
  rebooting anyway.

  It is guarded on **content**, not on whether anything wrote. Booting CMD's
  DOS touches the card and changes none of its bytes, so a touch-based test
  would rewrite the image every session. And it writes through `ramlink.tmp`
  and renames, so a failed save leaves the previous image intact.

The load happens in the same phase as ROM loading, while the SD is still
mounted, which is **before** `CSuperCPU::init()` runs. `init()` fits the card
again — so `CRAMLink::init()` treats re-fitting the *same* size as a no-op that
preserves contents rather than a reallocation that wipes them. That is both the
honest model of battery-backed RAM and the thing the boot path depends on;
`ramlink_card_image_survives_refitting_the_same_card` pins it, because the only
symptom of getting it wrong would be a blank card the user blames on their image
file.

## Configuration

`RAMLINK_SIZE` in `SCPU/scpu.cfg` — a selector, not a size:

| Value | Card |
|---|---|
| `1` | no RAMLink (stock upstream default) |
| `2` | 1 MB |
| `3` | 2 MB |
| `4` | 4 MB |
| `5` | 8 MB — **this fork's default** |
| `6` | 16 MB |

Fitting one also wants `SCPU/ramlink.rom`, the 64K EPROM. `make sdcard` stages it
from `ROMs/ramlink201.bin` if present. Without the image the unit still answers
its registers — what an empty socket does — and startup says so rather than
failing to boot.

## Where it sits in the I/O chain

```
CC64Memory -> CIOInterceptorChain -> CSuperCPURegisters   (primary)
                                  -> CRAMLinkInterceptor
                                  -> CREUInterceptor
```

The order is not arbitrary. The accelerator's own registers must be asked first,
because they observe the `$DF7E`/`$DF7F` strobes for `$D0BC` bit 6 and the vector
reroute — and then **decline**, so the write continues down the chain and the
RAMLink device sees it too. A chain that stopped at the first handler would give
one of the two and look like it worked;
`ramlink_strobe_reaches_both_the_accelerator_and_the_device` pins it.

RAMLink is ahead of the REU because that is where it sits in a real machine: it
claims I/O2, and the REU lives behind it in the RAM-Port.
