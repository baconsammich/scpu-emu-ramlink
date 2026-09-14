# SCPU-EMU 00.07.06-RL — the RAMLink fork

This is SCPU-EMU with a CMD RAMLink **fitted and switched on**, a RAMCard image
loaded from the SD card at startup, and — new in this release — CMD's own DOS
actually finding the device.

Upstream carries the RAMLink device but leaves it off, because a stock SuperCPU
card should behave like a stock SuperCPU card. This fork is the other choice.

**Fork point:** `00.03.00`. See `git log main..ramlink` in the source tree for
exactly what differs (26 commits, 22 files, +2111/−249).

## What is in the box

| | |
|---|---|
| `SDCard/` | Copy the **contents** onto a blank FAT32 card. Complete and bootable. |
| `scpu-emu-00.07.06-RL-sdcard.zip` | The same, zipped. |
| `scpu-emu-00.07.06-RL-src.tar.gz` | Source only — no ROMs, no card image, no build output. |
| `reports/` | The output of every gate this release had to pass. |
| `MANIFEST.md` | Every file on the card with its size and SHA-1. |

## New in 00.07.06-RL

- **The machine's speed is measured, not estimated.** `SCPU/busdiag-e18.txt`
  now carries the interpreter benchmark, the A53 PMU counters, and an
  end-to-end `achieved-khz` figure, every boot. Measured here: **9.7 MHz**.
  See "Where the time goes" in the README for what that is made of and why
  the interpreter is not the part holding it back.
- **Every ROM now has a documented source.** The three Commodore ROMs ship with
  VICE (verified byte-identical); SuperCPU DOS 2.04 is a free download from
  Zimmers; the RAMLink EPROM and a RAMCard image have an archive link, with
  SHA-1s beside it so what you download can be checked rather than assumed.
  `SCPU/PUT-ROMS-HERE.txt` in the ROM-free card payload carries all of it.
- **A 16 MB REU in the RAMLink's RAM-Port**, alongside the 16 MB RAMCard.
  `REUSIZE 7`. See the known limitations — it works, but it does not yet match
  VICE exactly at this size.
- **C64 DRAM powers up with a pattern, not zeros.** Real DRAM comes up in
  bands and software reads it; CMD's DOS reads uninitialised RAM during its
  RAMLink setup and acts on what it finds. This now reproduces VICE's pattern
  exactly — 4-byte bands offset by two, inverted every 16 KB, taken from
  VICE's runtime resource defaults rather than the struct initialisers in
  `ram.c`, which differ and are not the ones used.
- **Boot credits** now name everyone whose code is in the binary — Scott
  Hutter for scpu-emu, Carsten Dachsbacher for RAD, Rene Stange for Circle,
  the VICE team, CMD, and Patrick Bass / baconsammich for this fork.
- **A 16 MB RAMCard**, with the existing partition layout carried over.
- **The card can be saved back to SD** — `RAMLINK_PERSIST 1`, on the button press.
- `Tools/mkramcard.py`, because a RAMCard image cannot simply be padded.
- Two documentation errors corrected, both of which had been actively
  misleading; they are described where they were wrong rather than quietly
  fixed.

## What this fork fixes in the base SuperCPU emulation

Not everything here is about the RAMLink. These are corrections to the
emulator itself, measured against the upstream `00.01.00` tree, and they apply
whether or not a RAMLink is fitted:

- **The DOS extension's bank-1 mapping was writable.** It is read-only on the
  hardware — VICE's `R1` entry installs a read function and leaves the write
  hook at its preset, which is bank 0. This was live in ordinary operation:
  CMD's DOS enables the extension **269 times during a normal boot with no
  RAMLink attached**, and bank 1 `$8000-$9FFF` holds code it is about to run.

- **Undefined behaviour in the VIC renderer.** `cellPixels + copyLeft -
  cellLeft` evaluates left to right, so the pointer moved out of the object
  before coming back. Found by UBSan; fixed with parentheses.

- **Reads of files larger than a few hundred KB hung the Pi.** One `f_read`
  becomes one `disk_read` of the whole file, and Circle's EMMC driver does not
  survive 16384 sectors. The symptom was a machine that came up with no
  accelerator and no cursor, with nothing to connect it to the file being read.
  Reads and writes are now chunked at 64KB.

- **Member initialisers in the 65816 core ran out of declaration order**, and
  the core carried an unused `eEntry` that suggested a check that was not
  happening.

- **`CFastRAM` and `CREU` owned heap allocations with implicit copy
  constructors** — a shallow copy would have double-freed.

- **The REU was implemented but never wired in.** `CC64Memory` takes a single
  `IIOInterceptor` and the accelerator's register file already held it; the
  chain in `reu_wiring.h` is what lets both exist.

- **ROM discovery**: `rom_paths.h` resolves logical names to the canonical
  part-number dumps, which un-skipped 11 integration tests that had been
  silently passing by not running.

The host test suite went from 9,343 to 10,499 lines over the same span, and the
65816 core is now checked against 5,080,000 SingleStepTests vectors with every
deviation from the reference accounted for.

## The headline: the RAMLink is found

In 00.04.00-RL, CMD's DOS booted perfectly and reported no RAMLink. There was no
error and no hang — the device was simply invisible, which is the hardest kind
of bug to go looking for. `OPEN1,16,15` gave DEVICE NOT PRESENT.

It now boots to `READY.` with the RAMLink attached, installs all 22 KERNAL
vectors, and on real hardware **produces a directory listing from the card**.

(`OPEN1,16,15` also returns cleanly — but that turns out to prove very little.
It returns cleanly on a completely blank card too, so it is not a test of
whether the device is configured. The directory listing is the evidence.)

The whole thing hinged on one byte. CMD's boot ROM asks whether a RAMLink is
there like this:

```
$031C  STA $D0BE     DOS extension ON
$031F  LDA $E0AB     <-- must read the RAMLink's ROM, not the accelerator's
$0322  STA $D0BF     DOS extension OFF
$0325  CMP #$7E      ramlink201.bin[$A0AB]
$0327  BNE $038F     taken -> "no RAMLink"
```

We answered `$C8` from the accelerator's own boot ROM and took the branch.

### What the DOS extension actually does

It had been modelled as two bank-1 windows, `$1000-$5FFF` and `$8000-$9FFF`.
That is right in most configurations, which is why nothing looked wrong. But
while the boot ROM is mapped **and** the hardware registers are enabled, it does
something else entirely: it drops the accelerator's EPROM and replaces it with
an Ultimax view of the expansion port. That is the only way CMD's boot code can
see a cartridge's ROM at all.

Taken from VICE's `scpu64` configuration table rather than guessed — the
`F8 → UM` transition happens exactly when bootmap=1, hwenable=1, game=1,
exrom=0, i.e. when a cartridge is holding the port in Ultimax, which a RAMLink
always does.

## Fixes that matter even without a RAMLink

**The DOS extension's bank-1 mapping is read-only.** Writes through those
windows go to ordinary bank-0 RAM; VICE's `R1` entry installs a read function
and leaves the write hook at its preset, and says so in a comment. We were
routing writes to bank 1 as well.

This was live on real hardware in ordinary operation, not just on the RAMLink
path: CMD's DOS enables the extension **269 times during a normal boot with no
RAMLink attached**. Bank 1 `$8000-$9FFF` is where RL-DOS lives, and the DOS
writes that range as ordinary RAM throughout its init — so the machine was
quietly dismantling code it was about to run.

**The 8 MB card image no longer hangs startup — confirmed on hardware.** A
single `f_read` of 8 MB becomes one 16,384-sector `disk_read`, which Circle's
EMMC driver does not survive; the symptom was a Pi that never reached its second
ACT-LED milestone, so the C64 came up with no accelerator and no cursor. Reads
are now chunked at 64 KB. The same card that used to hang has now been booted
with the 8 MB image present and with it renamed away, with identical results.

## RAMLink device fixes

- **The `$DE00` window decodes whether or not a source has been selected.** The
  power-on source is 7 — "unused" — and CMD's KERNAL touches the window before
  it writes `$DFC0`. Declining sent those reads to the C64 and wedged the
  RAMLink KERNAL at `$FAF6`.
- **The cartridge KERNAL never displaces the accelerator's.** CMD's ALT KERNAL
  runs `STA $DF7E` / `JSR $FE98` / `STA $DF7F` — switching RAMLink on and then
  immediately executing the next instruction out of that same window. Serving
  the cartridge there means the `JSR` is fetched from RAMLink's ROM and the
  machine leaves for somewhere else.
- **The i8255A returns the output latch for ports configured as outputs**, which
  is how software read-modify-writes a port it drives.
- **Port C bits 7-6 arrive inverted.** They are the CMD parallel bus's handshake
  lines; with no drive attached the bus idles high, so they read 0.

## Testing

**Suite: 654,948 checks, 0 failed** — clean under ASan + UBSan.

Cartridge I/O is now diffed access-by-access against a real VICE 3.10 `xscpu64`
instrumented at its own `scpu64io_de00/df00` dispatch. Ours tracks VICE's
reference for **348 consecutive accesses** through the probe, the PPI handshake,
the rambase walk and the RTC setup. The one remaining difference is inside an
REU-presence probe, where VICE's open bus happens to match the written value for
a few iterations and ours mismatches on the first; both reach "no REU", which is
the answer that matters, and the iteration count is a property of what the VIC
last fetched rather than something reproducible between implementations.

## Tested on

Everything claimed here as "runs on hardware" was run on this, and only this:

| | |
|---|---|
| Computer | Commodore 64 "breadbin", **250407** motherboard |
| Cartridge | RAD Expansion Unit with a **Raspberry Pi 3A+** |
| Power | External supply to the RAD |
| SD card | 32 GB, FAT formatted |

Worth saying plainly because the list is short. A 250407 is an early-ish
board, and board revision matters on this project more than it usually would:
the timing constants in `Config/default.cfg` were tuned against real bus
behaviour, and a different revision, a different Pi, or bus power instead of
external power are all variables nobody has swept. A C128 path exists in the
code and has never been run.

If it misbehaves on other hardware that is a finding, not a surprise — the
`WAIT_*` family in `scpu.cfg` is where it would show, and
`SCPU/busdiag-e18.txt` on the card records what the bus self-test actually
measured on your machine.

## Known limitations

Everything upstream 00.03.00 lists, plus:

- **Reads are proven; writes are not.** On a real C64 through a RAD, 00.05.00-RL
  boots with the RAMLink fitted and **produces a directory listing from it**.
  That exercises the whole read path end to end — the DOS wedge taking the
  call, the `$DE00` window returning real card data, card addressing, and the
  image's own directory structure. Nothing below this line has been through a
  real machine.
- **A 16 MB REU diverges from VICE, in a way that is visible but not harmful.**
  With a 512 KB REU, VICE and this emulator agree exactly: both write the same
  349 bytes to the RAMCard during boot. With a 16 MB REU, VICE instead sets up
  an REU-to-RAMCard transfer — `$DF01 = $B1`, 256 bytes to `$DE00` — and
  leaves the card untouched. We skip that command and fall back to writing the
  card by hand, so the card ends up 349 bytes different where VICE's is
  unchanged.

  Every one of the 6000 logged cartridge accesses matches VICE on address, so
  the cause is a data difference, not a wrong decode. It is upstream of the
  REU: at the moment CMD's DOS makes the choice, VICE's bank-0 SRAM holds `$AA`
  across the region we still have at its power-on pattern — something writes it
  there in VICE and not here, and that is not yet explained.

  It is **not** corruption: the 349 bytes are exactly what a 512 KB REU
  legitimately writes, and VICE writes them too in that configuration. Set
  `REUSIZE 4` for the configuration that matches VICE byte for byte.

- **Writing, saving, partitioning and formatting are unexercised.** The register
  file and the window are modelled from VICE and pinned by tests, but no test
  and no hardware run has yet written a file to a RAMLink partition and read it
  back. A device that lists is not yet a device that stores.
- **`LOAD"$",16` still does not complete under `scpu_trace`** — but neither does
  `LOAD"$",8` with no RAMLink in the machine, while `OPEN15,8,15` does. That is
  the harness having nothing on the serial bus to answer a LOAD, and hardware
  has now settled the question the harness could not.
- **The RAMCard can now be saved, but that path is untested on hardware.** Set
  `RAMLINK_PERSIST 1` and the card is written back to `SCPU/ramlink.img` on the
  button press that reboots the Pi — guarded on content rather than on whether
  anything wrote, and written through a temporary that is renamed over the
  target so a failure leaves the previous image intact. Off by default.
  Whether a BASIC `SAVE` to device 16 actually reaches the card is the open
  question; the host harness cannot answer it (see below).
- **The supplied card image is formatted and readable.** This was an open
  question — the "CMD"/"RAMLINK" strings in it are a device-name table inside
  code, not a partition label, and the first sectors are a `ff 00 00 ff` fill
  pattern, so static inspection could not settle it. A directory listing off
  real hardware did.

- **The card is 16 MB in this release, up from 8 MB.** An image cannot simply be
  padded to a larger card: RAMLink keeps its system partition at the TOP, at
  `size - $1000`, so a padded 8 MB image leaves it stranded in the middle and
  the card reads as unformatted (measured: 333 bytes rewritten at boot, versus 0
  for a correct image). `Tools/mkramcard.py` relocates it, and `make sdcard`
  picks the image to stage from `RAMLINK_SIZE`. The original 8 MB layout —
  C64OS, two 1541 partitions and two 1581s — is carried over intact.
- **No CMD HD on RAMLink's parallel bus.** The i8255A latches behave correctly so
  a probe fails cleanly, which is the honest answer with no drive attached.
- **The DOS ROM is not banked into `$8000-$BFFF`** — that is what the hardware
  does on an accelerated machine, not a shortcut. The SuperCPU's own DOS carries
  RAMLink support, and VICE declines that mapping for SCPU64 explicitly.

## Installing

Copy the **contents** of `SDCard/` to a blank FAT32 card — files in the root,
not inside an `SDCard` folder. Keep `SCPU/` as a subdirectory.

If the machine does not boot: set `BOOTMAP 0` in `SCPU/scpu.cfg`. If it boots
but misbehaves, set `RAMLINK_SIZE 1` to take the RAMLink out of circuit
entirely, which makes it a stock-behaving card again. That file is read before
any emulation starts, so it is always a safe recovery path.

To see what the firmware itself thinks happened, plug a monitor into the **Pi's**
HDMI — `VIDEO_MODE 0` keeps its text console there, and the RAMCard load prints
`RAMLink: ramlink.img loaded, N of M KB` during startup.

## Legal

Commodore ROMs are copyright Commodore International Corporation. The SuperCPU
and RAMLink ROMs are copyright CMD. CMD is a trademark of Creative Micro
Designs. The ROM images and the RAMCard image on the card were staged from this
machine's own `ROMs/` directory — **the card payload is not redistributable**.
The source tarball excludes them.

SCPU-EMU is GPLv3, inherited from RAD-Doom by Carsten Dachsbacher.
