# ROMs

**No ROM images are committed to this repository. The four runtime images are
required on the SD card.**

## Local development set

For testing, this directory is populated locally (and git-ignored). To recreate
it:

```sh
C=http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/c64
curl -sSLO $C/kernal.901227-03.bin      # sha1 1d503e56df85a62fee696e7618dc5b4e781df1bb
curl -sSLO $C/basic.901226-01.bin       # sha1 79015323128650c742a3694c9429aa91f355905e
curl -sSLO $C/characters.901225-01.bin  # sha1 adc7c31e18c7c7413d54802ef2f4193da14711aa
cp kernal.901227-03.bin kernal.rom
cp basic.901226-01.bin  basic.rom
cp characters.901225-01.bin chargen.rom

M=https://www.zimmers.net/anonftp/pub/cbm/firmware/misc/cmd
curl -sSLO $M/scpu-dos-2.04.bin         # 128K SuperCPU DOS, sha1 6aa529a7b1b6de53e8979e407a77b4d5657727f5
curl -sSLO $M/scpu-dos-1.4.bin          # 128K, earlier revision, sha1 3422a7735f0f959d990ab39512d8815a5d8eab7a
cp scpu-dos-2.04.bin scpu.rom
```

`Tests/Integration/test_real_kernal.cpp` uses `kernal.rom` and `basic.rom` to
boot a genuine KERNAL through the CPU core. Those host-side tests report as
*skipped* rather than failing when the local files are absent, so a fresh clone
still goes green. That development convenience does not change the firmware's
runtime requirement.

The SuperCPU DOS images are 128KB. `make sdcard` stages **2.04** as
`SCPU/scpu.rom`, and the firmware maps it at `$F80000`. The runtime preflight
requires this exact 128KB image along with the three C64 ROMs. An incomplete set
is reported on HDMI and leaves the physical machine to boot normally.

**The two images are for different machines.** This is the thing to get right:

| | machine | contents |
|---|---|---|
| `scpu-dos-1.4.bin` | **SuperCPU 64** | first 64KB populated, rest `$FF`. No C128 content. |
| `scpu-dos-2.04.bin` | **SuperCPU 128** | all 128KB, and carries a C128 KERNAL and BASIC — the strings `(C)1986 COMMODORE ELECTRONICS, LTD.` and `(C)1977 MICROSOFT CORP.` are in there. |

**Correction — an earlier version of this file was wrong here.** It said that
staging 2.04 on a C64 gets you to the SuperCPU boot screen and then
`SUPERCPU INITIALIZATION ERROR: 06` "because its boot code is checking for a
machine that is not there." Both halves are wrong:

- **2.04 works.** That attribution was made during an early debugging round and
  was retracted on 2026-08-04; the real causes were emulation bugs since fixed
  (KERNAL shadow, `$D0B2` window moves). `make sdcard` stages 2.04, and that is
  deliberate — do not "helpfully" revert it to 1.4.
- **Error 06 is not a machine-detection failure.** On real hardware it is the
  documented symptom of an **inadequate power supply** (CMD SuperCPU FAQ). The
  accelerator already exceeds the nominal cartridge-port current specification
  on its own. It has nothing to say about which DOS image is installed.

Both corrections point the same way: if error 06 appears, suspect power on
hardware and the emulation elsewhere — not the ROM version.

1.4's boot chain, for reference: reset reads `$FFFC` under bootmap and gets
`$FC90`, which is `JML $F800FC`, which is `JML $F80100`, which is the real
start — `SEI`, set up the stack, then enable the hardware registers at `$D07E`.
So both the bootmap window and the `$F80000` mapping have to be right for it to
get anywhere.

`BOOTMAP 1` maps this ROM over bank 0 at reset so its code runs before the C64
KERNAL, matching the real accelerator. `BOOTMAP 0` remains the SD-card recovery
setting if that boot path is being diagnosed.

## RAMLink

`ramlink.rom` is the 64KB CMD RAMLink EPROM, and is **optional** -- it only
matters when `RAMLINK_SIZE` in `scpu.cfg` fits a card. `make sdcard` stages it
from `ramlink201.bin` (v2.01) or `ramlink140.bin` (v1.40) if either is present.

The image is four 16KB banks: 0 and 1 are the RAMLink DOS, selected through the
i8255A's port C; bank 2 is the C64 KERNAL replacement and carries JiffyDOS 6.01;
bank 3 is the C128 one. Only the DOS banks matter here -- on an accelerated
machine the ROM is not banked into `$8000-$BFFF` at all. See
[../Docs/RAMLink.md](../Docs/RAMLink.md).

A RAMCard image is a raw dump of the card's RAM, one byte per byte, sized to the
card. Nothing loads one automatically yet; see the persistence note in
Docs/RAMLink.md.

## Required runtime set

Place all four files in `SCPU/` on the SD card:

| File | Size | Notes |
|---|---|---|
| `kernal.rom` | 8192 | C64 KERNAL 901227-03 |
| `basic.rom` | 8192 | C64 BASIC 901226-01 |
| `chargen.rom` | 4096 | C64 character ROM 901225-01 |
| `scpu.rom` | 131072 | SuperCPU DOS 2.04, mapped at `$F80000` |

Optional:

| File | Size | Notes |
|---|---|---|
| `ramlink.rom` | 65536 | CMD RAMLink EPROM. Only with `RAMLINK_SIZE` set. |

If any file is absent or has an unusable size, startup prints the complete
required-file checklist, does not assert `/DMA`, and reports `BOOTING NORMALLY`.
`scpu.rom` is the real SuperCPU ROM image — SuperCPU DOS, which brings JiffyDOS
with it.

## Why chargen is different

Exposing the character ROM needs CHAREN low, and with the 6510 held off the bus
nothing can rewrite its I/O port. So it cannot be captured the way BASIC and
KERNAL are.

This only affects programs that read the character set *through the CPU*. The
VIC-II fetches it directly on the C64 side and is unaffected, so the display is
correct either way. A future option is injecting a short 6502 stub over Ultimax
to copy it into RAM before takeover — see
[../Docs/SuperCPU64/supercpu-memory-map.md](../Docs/SuperCPU64/supercpu-memory-map.md).

## Legal

Commodore ROMs are copyright Commodore International Corporation. The SuperCPU
ROM is **copyright CMD**. CMD is a trademark of Creative Micro Designs.

Dump them from hardware you own, or obtain them from a source you are entitled
to use. Do not commit them here — `.gitignore` is set up to prevent it.

The SuperCPU ROM is a free download from Zimmers —
<https://www.zimmers.net/anonftp/pub/cbm/firmware/misc/cmd/>.

## RAMCard images

`ramlink.rl` is an 8MB RAMCard dump; `ramlink16.rl` is the same card relocated
to 16MB by `Tools/mkramcard.py`. The image staged onto the SD card is chosen
from `RAMLINK_SIZE` in `Config/default.cfg`, because an image has to MATCH the
fitted card rather than merely fit inside it — RAMLink keeps its system
partition at the top of the card, so a padded image leaves it stranded in the
middle and the card reads as unformatted.

    Tools/mkramcard.py ROMs/ramlink.rl ROMs/ramlink16.rl 16

Both are dumps of a configured CMD card and are not redistributable.

## Sourcing the ROMs

SCPU-EMU ships no ROMs and never will — they are Commodore's and CMD's
property, not this project's to give away. You supply your own.

### What the card needs

The firmware looks for these **exact filenames** in `SCPU/` on the card. Rename
your dumps to match; the part-number names on the right are what the files are
usually called in the wild.

| On the card | Bytes | Usually named | SHA-1 |
|---|---:|---|---|
| `SCPU/basic.rom` | 8192 | `basic-901226-01.bin` | `79015323128650c742a3694c9429aa91f355905e` |
| `SCPU/kernal.rom` | 8192 | `kernal-901227-03.bin` | `1d503e56df85a62fee696e7618dc5b4e781df1bb` |
| `SCPU/chargen.rom` | 4096 | `chargen-906143-02.bin` | `0fad19dbcdb12461c99657b2979dbb5c2e47b527` |
| `SCPU/scpu.rom` | 131072 | `scpu-dos-2.04.bin` | `6aa529a7b1b6de53e8979e407a77b4d5657727f5` |

Optional, and only meaningful with `RAMLINK_SIZE` set:

| On the card | Bytes | Usually named | SHA-1 |
|---|---:|---|---|
| `SCPU/ramlink.rom` | 65536 | `ramlink201.bin` | `01e2f634c7464e5749c464b59f4c590903a01bd5` |
| `SCPU/ramlink.img` | card-sized | a RAMCard dump | — |

The SHA-1s are what this emulator was developed and tested against. A different
KERNAL revision will generally work — JiffyDOS certainly does — but if
something behaves oddly, checking you have these exact images removes one
variable.

If the first four are not all present the RAD does not take the bus and your
Commodore boots normally. That is the intended failure: a machine that will not
start is worse than one that starts without the accelerator.

### Where to get them

**The three Commodore ROMs ship with VICE.** This is the easiest legitimate
route and needs no dumping hardware. Install [VICE](https://vice-emu.sourceforge.io/)
and look in its data directory:

```
<vice>/data/C64/basic-901226-01.bin
<vice>/data/C64/kernal-901227-03.bin
<vice>/data/C64/chargen-906143-02.bin
```

On macOS that is inside the app bundle; on Linux typically
`/usr/share/vice/C64` or `/usr/lib/vice/C64`; on Windows, `C64\` beside the
executables. **Verified: VICE 3.10's copies of all three are byte-identical to
the images above.**

**Dump them from your own hardware.** Always legitimate, and the only route
that gives you exactly the revisions your machine has. Any of the common
cartridge-based dumpers will do it.

**The SuperCPU DOS ROM is a free download.** Zimmers, the long-running CBM
archive, carries it at
<https://www.zimmers.net/anonftp/pub/cbm/firmware/misc/cmd/>:

```
scpu-dos-2.04.bin   131072 bytes   ->  SCPU/scpu.rom
scpu-dos-1.4.bin    131072 bytes       the documented fallback
```

Both are 131072 bytes, matching the size this emulator expects. 2.04 is the
image everything here was developed and tested against; 1.4 boots but fails its
own SRAM test on this emulator and is kept only as a comparison point.

**Archives.** The Commodore ROMs are widely mirrored too — the same Zimmers
tree, archive.org, and the various C64 preservation sites.

**The RAMLink EPROM and a RAMCard image.** Neither is in the Zimmers CMD
directory — CMD is long gone and the EPROM was never sold separately. One
archive that has been used for both:

```
https://www.mediafire.com/file/empvwwbu2oquuct/ramlink.zip   334 KB, Dec 2022
```

Two honest caveats. It is a file-locker upload, not a curated archive: nobody
vouches for what is in it, and the link can stop working at any time. And this
project has not verified its contents — what can be said is that `ramlink.rl`
in this directory carries a 1 December 2022 date, the same day that file was
uploaded, and 334 KB compressed is about right for a 64 KB EPROM plus an 8 MB
RAMCard image of highly repetitive data.

So check what you download rather than assuming:

```
shasum ramlink201.bin   -> 01e2f634c7464e5749c464b59f4c590903a01bd5   65536 bytes
shasum ramlink.rl       -> a136863c39c0974d246099af755c8689b5494c4f   8388608 bytes
```

Those are the images this emulator was developed and tested against. A
different RAMLink ROM revision may well work — 2.01 is simply the one that has
been through every test here.

Dumping from a RAMLink you own remains the route with no questions attached.

### Why the SHA-1s are here

Not as a checksum ritual. A wrong-sized or wrong-revision ROM produces failures
that look like emulator bugs — a machine that boots to a garbage screen, or one
that works until some specific KERNAL call. Being able to rule the ROMs out in
ten seconds is worth the table.
