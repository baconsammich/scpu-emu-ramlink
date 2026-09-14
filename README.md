# SCPU-EMU — RAMLink fork

> **This is the RAMLink fork.** It is SCPU-EMU 00.03.00 plus a CMD RAMLink that
> is **fitted and switched on by default**, and that loads a RAMCard image off
> the SD card at startup. Build string `00.04.00-RL`, so a card carrying it is
> distinguishable from a stock one at a glance.
>
> Upstream fits no RAMLink unless you ask for one. Here an 8 MB card is fitted
> out of the box and `SCPU/ramlink.img` is read into it. Set `RAMLINK_SIZE 1` in
> `SCPU/scpu.cfg` and the device decodes nothing — the machine is then exactly
> a stock 00.03.00 machine, which is the way to find out whether RAMLink is
> responsible for something.
>
> Everything else is unchanged from upstream. See
> [Docs/RAMLink.md](Docs/RAMLink.md) for what the device does and does not do,
> and `git log main..ramlink` for exactly what this fork changed.
>
> **Runs on hardware, reads and writes.** Booted on a Commodore 64 breadbin
> (250407 board) through a RAD Expansion Unit with a Pi 3A+ and external power,
> from a 32 GB FAT card: the machine comes up with the RAMLink fitted, CMD's
> own DOS finds the device, it produces a directory listing, and a RAMCard
> saved back to SD came out byte-identical to what the emulator produces from
> the same starting image — 349 bytes changed, same ten regions, all 16 MB
> equal.
>
> That is one board, one Pi, one card. Board revision matters here: the timing
> constants were tuned against real bus behaviour and nobody has swept other
> hardware.

CMD SuperCPU emulation for the Commodore 64/128, running on a Raspberry Pi in a
[RAD Expansion Unit](https://github.com/frntc/RAD).

The Pi asserts `/DMA`, the C64's 6510 stops driving the bus, and the Pi becomes
the machine's CPU — emulating a WDC 65C816 out of its own fast RAM and only
touching the C64 for I/O and to keep VIC-visible memory coherent. That is the
same manoeuvre the real CMD SuperCPU performs.

Derived from [RAD-Doom](https://github.com/frntc/RAD-Doom) by Carsten
Dachsbacher, which established that a Pi can replace a C64's CPU over the
expansion port. GPLv3.

> **Status: it works.** A real C64 runs with the Pi as its 65816 — booting
> CMD's own SuperCPU DOS ROM to its splash and banner, JiffyDOS KERNAL, 16MB of
> SuperRAM detected by CMD's own code, stable display, and working IEC disk
> access against real drives. **Measured at 9.7MHz** against the 20MHz target,
> and the interpreter is not what is holding it back — that clears 20MHz on its
> own. See "Where the time goes" below.
>
> On the host side the same stack runs on a PC: the CMD ROM boots against a
> fake IEC drive that serves a directory over the full slow serial protocol
> (`Tools/host_cmdboot/`), and the 65816 core agrees with the 6502 core
> instruction for instruction across half a million checks.
> See [Docs/roadmap.md](Docs/roadmap.md).

## Hardware

- A RAD Expansion Unit ([build info](https://github.com/frntc/RAD))
- Raspberry Pi 3A+, 3B+ or Zero 2
- A PAL C64 or C128 (NTSC is modelled but unvalidated)

## Quick start

```sh
make tests          # host test suite — needs only g++, no hardware
make firmware       # Raspberry Pi kernel image — needs a Circle tree
```

See [Docs/build.md](Docs/build.md) for the Circle setup, and
[Docs/hardware-setup.md](Docs/hardware-setup.md) for the SD card layout.

## ROMs

None ship with this project. Firmware startup requires `basic.rom`,
`kernal.rom`, `chargen.rom`, and the 128KB SuperCPU DOS 2.04 image as
`scpu.rom` under `SCPU/` on the SD card. If the set is incomplete, the RAD does
not acquire the bus and the physical Commodore boots normally.

`ramlink.rom` (64KB, the CMD RAMLink EPROM) is **required in this fork**, which
fits a RAMLink by default. `ramlink.img`, a raw RAMCard image, is optional — a
card with no image comes up blank and has to be formatted. See
[ROMs/README.md](ROMs/README.md).

### Where to get them

**The three Commodore ROMs ship with VICE** — the easiest route, and it needs no
dumping hardware. Install [VICE](https://vice-emu.sourceforge.io/) and take them
from its data directory:

```
<vice>/data/C64/basic-901226-01.bin    -> SCPU/basic.rom
<vice>/data/C64/kernal-901227-03.bin   -> SCPU/kernal.rom
<vice>/data/C64/chargen-906143-02.bin  -> SCPU/chargen.rom
```

On macOS that is inside the app bundle; on Linux usually `/usr/share/vice/C64`;
on Windows, `C64\` beside the executables. VICE 3.10's copies are byte-identical
to the images this emulator was tested against.

**Dumping from your own hardware** always works and gives you the exact
revisions your machine has.

**The SuperCPU DOS ROM is a free download** from Zimmers, the long-running CBM
archive:

```
https://www.zimmers.net/anonftp/pub/cbm/firmware/misc/cmd/
  scpu-dos-2.04.bin   131072 bytes  ->  SCPU/scpu.rom
  scpu-dos-1.4.bin    131072 bytes      (the documented fallback)
```

**Archives** — Zimmers (`zimmers.net/anonftp/pub/cbm/`), archive.org and the
C64 preservation sites also mirror the Commodore ROMs.

**The RAMLink EPROM and a RAMCard image** are not in the Zimmers CMD directory.
One archive that has been used for both:

```
https://www.mediafire.com/file/empvwwbu2oquuct/ramlink.zip   (334 KB, Dec 2022)
```

That is a file-locker upload rather than a curated archive — its provenance
cannot be checked and the link may stop working. Verify whatever you get
against the SHA-1s in [ROMs/README.md](ROMs/README.md) before trusting it.
Dumping from a RAMLink you own remains the route with no questions attached.

[ROMs/README.md](ROMs/README.md) has the exact filenames, sizes and SHA-1s.

Commodore ROMs are copyright Commodore International Corporation. CMD is a
trademark of Creative Micro Designs.

## Configuration — `SCPU/scpu.cfg`

The card is configured by a single plain-text file, `SCPU/scpu.cfg`, staged from
[Config/default.cfg](Config/default.cfg) by `make sdcard`. One `KEY value` per
line, `#` for comments. The Pi reads it before any emulation starts, so a
setting always takes effect no matter how badly the emulated machine behaves —
which is what makes it a safe recovery path.

**`Config/default.cfg` is the authoritative reference.** Every key carries a
comment there explaining what it does and why its default is what it is. The
table below covers the settings you would normally choose between; the rest are
timing and cache parameters that should be left alone unless you are working on
bus timing itself.

### Machine

| Key | Values | Notes |
|---|---|---|
| `JIFFYDOS` | `1` on, `0` off | The virtual replacement for the physical switch on a real SuperCPU. `POKE 53429,128` / `POKE 53429,0` changes it until the Pi reboots. |
| `REUSIZE` | `1` none, `2` 128K, `3` 256K, `4` 512K, `5` 2MB, `6` 4MB, `7` 16MB | RAM Expansion Unit. 1MB is deliberately absent — selectors are appended, never renumbered, so a card in the field cannot change machine because the table grew. |
| `CPU_CORE` | `1` 65816, `0` 6502 | `0` is a fallback: everything a normal C64 does still works, but SuperRAM is unreachable because a 6502 cannot name an address above `$FFFF`. Quickest way to find out whether the CPU core is responsible for a regression. |
| `BOOTMAP` | `1` on, `0` off | Runs the SuperCPU ROM at reset. Firmware startup requires the 128KB SuperCPU DOS 2.04 image at `SCPU/scpu.rom`. **If the machine does not boot, set this to 0.** |
| `C128_MODE` | `0` auto, `1` force C64 path, `2` force native C128 | `2` is experimental. On a C128 the physical machine type and the operating mode are different things, and `$0001` is internal to the 8502 so a DMA master cannot read it. |
| `RAMLINK_SIZE` | `1` none, `2` 1MB, `3` 2MB, `4` 4MB, `5` 8MB, `6` 16MB | CMD RAMLink. Fits the device — its registers, internal RAM, RAMCard and clock. Also wants `SCPU/ramlink.rom`. On an accelerated machine the RAMLink's DOS ROM is deliberately *not* banked in; the SuperCPU's own DOS carries that. See [Docs/RAMLink.md](Docs/RAMLink.md). |
| `BOOT_ANIMATION` | `1` on, `0` faithful | `1` runs the C64 startup animation that SuperCPU DOS 2.04 carries but skips. A deliberate one-byte deviation from strict fidelity. |

### Video

`VIDEO_MODE` selects what the **Pi's HDMI output** shows. It does not change how
the C64's own screen is driven — the physical VIC-II does that in every mode.

| Value | HDMI output |
|---|---|
| `0` | Firmware text console. The safe baseline; the runtime path is unchanged. |
| `1` | The VIC-II picture, rendered by the Pi from its memory shadow. |
| `2` | C128 VDC picture. Reserved for future native C128 support. |

> **`VIDEO_MODE 1` is experimental.** The Pi has to reconstruct the picture from
> its shadow while simultaneously meeting the C64's bus timing, and the two
> compete for the same scarce resource. Because of those timing and bandwidth
> limits it may not give the output you expect — expect artefacts, missing
> updates, or reduced fidelity on demanding software. `VIDEO_MODE 0` is the
> baseline to fall back to, and the mode to use when diagnosing anything else.

### Display mirroring

The Pi's shadow RAM is authoritative and physical DRAM is write-only, existing
purely so the VIC-II has something to fetch. These control how much of that
shadow gets pushed back out, and when.

| Key | Default | Notes |
|---|---|---|
| `MIRROR_DISPLAY_BYTES` | `1024` | Visible-display delivery allowance, roughly 8KB/frame, spread over 128 points so no single pause is long. `0` restores strict border-only mirroring. |
| `MIRROR_D000_RELOCATE` | `1` | Translates sprite shape pointers that land under `$D000-$DFFF`, which is DRAM to the VIC but I/O to any bus master and therefore unreachable. Without it, 3D Pool/SCPU's balls alternate between correct and garbage at frame rate. |
| `DISPLAY_SCRUB` | `0` | Targeted repair for sparse stored-data changes on bitmap screens. Text and charset modes are deliberately excluded. Leave disabled until bitmap-only hardware trials validate it. |
| `VECTOR_REROUTE` | `1` | Fetches interrupt vectors from the accelerator's ROM window under the conditions VICE models as `scpu64_interrupt_reroute()`. Ordinary C64 operation in emulation mode is unaffected. |

### Timing and caching

`IO_STRETCH`, the `WAIT_*` family and the `CACHING_*` family describe the bus
protocol and the Pi's cache behaviour. These are the values the whole design
rests on, and a wrong one produces symptoms — phantom keypresses, half
directories, corrupted loads — that look nothing like a timing problem. Read the
comments in `Config/default.cfg` and
[Docs/SuperCPU64/timing-notes.md](Docs/SuperCPU64/timing-notes.md) before
changing any of them.

## Third-party dependencies

Nothing is vendored. This records what to fetch and why.

### Circle

Bare-metal C++ environment for the Raspberry Pi. **Version 44.3**, which is what
RAD targets.

- <https://github.com/rsta2/circle>

Use RAD's Circle build settings; its README is explicit that other settings
probably will not work correctly. Not vendored because Circle is large, has its
own build configuration, and pinning it here would either fork it or silently
drift from what RAD expects. See [Docs/build.md](Docs/build.md).

### RAD Expansion Unit

- <https://github.com/frntc/RAD>
- <https://github.com/frntc/RAD-Doom>

GPLv3. The parts SCPU-EMU uses have been promoted into `Source/Bus/RAD/` with
attribution intact, rather than pulled in as a dependency, because they needed
renaming and decoupling from RAD's REU-specific state. What was taken and what
was changed is recorded in
[Docs/SuperCPU64/rad-notes.md](Docs/SuperCPU64/rad-notes.md).

### Toolchain

`aarch64-none-elf` GCC for the firmware; any host C++ compiler for the tests.

## Layout

```
Source/
  App/          start-up, Circle entry point
  CPU/          CPU cores, isolated behind cpu_bus.h
    M6502/      milestone-1 core and test oracle
    W65C816/    production 65C816 core
  C64/          PLA banking, shadowed bank 0
  SuperCPU/     registers, write mirroring, optimization modes
  REU/          RAM Expansion Unit
  RAMLink/      CMD RAMLink
  Bus/
    RAD/        timing-critical hardware code
    Host/       in-memory backend so everything above is testable on a PC
    C64Side/    6502 stubs injected over Ultimax
  Common/
Tests/          host test suite
Docs/
  SuperCPU64/   SCPU64 architecture and hardware research
  SuperCPU128/  SCPU128 hardware findings
```

## Where the time goes

Measured on hardware — Pi 3A+ at a verified 1,400,146,000 Hz, not throttled,
written to `SCPU/busdiag-e18.txt` at every boot. At 1.4GHz a 20MHz 65816 has
**70 ARM cycles per emulated cycle** to spend.

| path | arm/cycle | MHz | |
|---|---:|---:|---|
| bank-1 interpreter | 67 | **20.9** | meets the target |
| screen write | 80 | 17.5 | 88% |
| bank-0 dispatch | 106 | 13.2 | 66% |
| SuperRAM | 143 | 9.8 | 49% |
| physical I/O | 416 | **3.4** | 17% |

### What the machine actually achieves

The five loops above are isolated paths. The whole machine, measured end to end
over 6.0 seconds of real time at the BASIC prompt:

```
achieved-khz=9704          9.7 MHz, 49% of the 20MHz target
                           144 arm cycles per emulated cycle, against a 70 budget
emu=58,225,849 cycles in host-us=5,999,780
```

That is an **idle** workload — the KERNAL's interrupt handler scanning the
keyboard and flashing the cursor sixty times a second, which is I/O-heavy
relative to its compute. A program that stays in bank 1 doing arithmetic would
sit much closer to the 20.9MHz the interpreter can sustain; one hammering
`$D020` would sit closer to 3.4. 9.7MHz is one real number, not a ceiling.

It does confirm the older "~8-12MHz" estimate this README used to carry, which
turned out to be a good guess.

The same record shows where the traffic goes:

```
mirror-candidate writes reaching the bus   173,893 of 2,044,745   (8.5%)
physical bus transfers                     386,095  (64,351/second)
slow cycles                                210,665 of 58,225,849  (0.36%)
  of which IEC-throttled                   200,015
```

Coalescing is already doing most of the work — 91.5% of candidate writes never
reach the bus. The remaining 64,000 transfers a second are the cost, and they
are what any serious speed work has to reduce.

**The interpreter is not the bottleneck.** It clears 20MHz on its own, and the
PMU says why it is not the problem: 63,862 ARM instructions per 1000 emulated
cycles at **0.95 instructions per cycle** — near the ceiling for an in-order
A53, not stalling. An older note in `Source/Makefile` recording "~0.3
instructions per clock, stall-bound" described a real measurement of a much
earlier build and no longer holds.

Physical I/O costs **6× the interpreter**, and that is the whole story: every
write the VIC-II might read has to cross the expansion port. Making the
emulator faster means moving less traffic, not executing fewer instructions.

Two honest caveats about these numbers:

- They are five separate micro-loops, not a workload. What any given program
  achieves depends on its mix, and real software touches all five.
- `l1i-refill` and `l1d-refill` both read zero, which is an artefact rather
  than a result: the benchmark loop is seven opcodes, so it touches a tiny
  slice of the interpreter and fits cache trivially. Real code spread across
  many opcode handlers would not. `stepInner()` compiles to 25,280 bytes
  against a 32KB two-way L1I, so cache pressure is still worth measuring —
  this benchmark simply cannot see it.

## Why this is tractable

RAD is not a generic cartridge that happens to be programmable — it is a
Raspberry Pi that can be bus master on a C64, and RAD-Doom already used it to
replace the CPU. The gap between "run Doom on the ARM and POKE the results in"
and "run a 65816 on the ARM and POKE the results in" is smaller than it looks.

The hard part is not the CPU emulation. A 65816 interpreter on a 1GHz A53 clears
20MHz comfortably. The hard part is that every write the VIC-II might read has
to be pushed back over the expansion port at roughly 1µs each, against ~50ns per
emulated cycle. Managing that traffic is what the SuperCPU's optimization modes
were for, and it is what most of the code here is about. See
[Docs/SuperCPU64/architecture.md](Docs/SuperCPU64/architecture.md).

## Credits

- **Scott Hutter / xlar54** — original author of
  [scpu-emu](https://github.com/xlar54/scpu-emu).
- **Carsten Dachsbacher** — the [RAD Expansion Unit](https://github.com/frntc/RAD)
  and RAD-Doom, which this is built on and which contains the hard-won bus
  timing.
- **Rene Stange** — [Circle](https://github.com/rsta2/circle), the bare-metal
  environment this links against.
- **The VICE team** — whose SCPU64 and RAMLink implementations this emulator is
  verified against, and in places transcribed from.
- **CMD**, for the SuperCPU and the RAMLink.
- **Patrick Bass / baconsammich** — creator of the [SCPU/RAMLink emulation fork](https://github.com/baconsammich/scpu-emu-ramlink).

The same list is printed on the Pi's console at every boot, before the SD card
is even mounted, so it is visible whether or not startup succeeds.

Commodore, CMD and Raspberry Pi trademarks belong to their respective owners.

## License

GPLv3, inherited from RAD-Doom. See [LICENSE](LICENSE).
