# A working RAMLink, in VICE — and what it shows

This records a **working reference** for RAMLink on an accelerated machine, how
to reproduce it, and the one place our emulator diverges from it. Everything
below was measured, not inferred.

## Building a headless xscpu64

VICE publishes no macOS binaries, and `brew install vice` is a trap here: it
does not use bottles for this platform and starts building `llvm@22` from
source, pulling in meson, ninja, cargo-c, libgit2 and more. Build it directly
instead — VICE ships `src/arch/headless/`, including `scpu64ui.c`, so no GTK and
no display are needed.

```sh
curl -LO https://downloads.sourceforge.net/project/vice-emu/releases/vice-3.10.tar.gz
tar xzf vice-3.10.tar.gz && cd vice-3.10
./configure --enable-headlessui --disable-pdf-docs --without-pulse --without-alsa
make -j8          # produces src/xscpu64 and src/x64sc
```

Two prerequisites the configure script insists on:

* **`dos2unix`** — only used to normalise line endings. A three-line shell shim
  that runs `perl -pi -e 's/\r\n/\n/g'` and ignores its options is enough.
* **`xa65`** — the 6502 cross-assembler. Build from
  <https://github.com/fachat/xa65> (the `xa/` subdirectory); it takes a minute.

## Running it

**Option order matters.** `set_enabled()` in `ramlink.c` attaches the BIOS image
when the `RAMLINK` resource is set, so the filename must already be set:
`-ramlink` placed first is rejected with *"Option '-ramlink' not valid"*, which
looks like the option does not exist and is really an ordering error.

```sh
./src/xscpu64 -directory "$PWD/data" \
  -scpu64 scpu-dos-2.04.bin \
  -ramlinkbios ramlink201.bin -ramlinksize 8 \
  -ramlinkimage card.rl -ramlinkimagerw \
  -ramlink \
  -warp -keybuf-delay 300 -limitcycles 250000000 \
  -exitscreenshot out.png -keybuf '10 open15,16,15
20 input#15,a,b$,c,d
30 print a;b$;c;d
40 close15
run
'
```

`-keybuf` takes **lowercase**: a C64 in uppercase/graphics mode renders typed
uppercase as PETSCII graphics, so `LOAD` arrives as `L♦=`. And `INPUT#` cannot
run in direct mode, hence the numbered program rather than three typed lines.

## The result, and what it proves

On a plain C64 the command channel answers:

```
 73 CMD RL DOS V2.01 0  0
```

On **xscpu64**, with the same RAMLink and the same card:

```
 73 CMD RL DOS V2.06 0  0
```

**Different version strings from the same RAMLink.** 2.01 is RAMLink's own BIOS;
2.06 is the copy carried in the SuperCPU's ROM. That is direct confirmation that
on an accelerated machine RL-DOS comes from the accelerator, not from the
cartridge — which is what `$8000-$9FFF`/`$1000-$5FFF` RLDOS in the bank-1 mirror
table, and startup error `#05` "RL-DOS ROM Checksum Error", both say.

VICE also auto-configures a blank card, writing the same structure our supplied
image has: device number `$10` at `$7FF5E1`, `RAMLINK` at `$7FF5F0`, a `SYSTEM`
partition and `RAMLINK  1` at `$7FF800`.

## Where we diverge

`scpu64_d200_store()` instrumented to log address, value and PC gives the
reference boot:

```
D2 store d200 = 00  pc=$0175
D2 store d200 = c0  pc=$032d      <- LDA #$C0 / STA $D200, then STA $DF7E
D2 store d203 = 00  pc=$fe4f
D2 store d204 = 00  pc=$fe52
D2 store d202 = 80  pc=$fe6a      <- the gate flag, bit 7 SET
```

Ours writes `$D200 = $00` at `$0173` and then never reaches `$032B`, so the
RAMLink init at `$0032E` (`STA $DF7E`) never runs and `$D202` bit 7 is never
set. Both boots agree up to `$0173`; they part at the `JSR $0574` two
instructions later, whose result selects the branch at `$00179`.

The boot code just before that point is worth reading, because it is 65816 long
addressing and disassembles wrongly without it:

```
$00151: A2 00        LDX #$00
$00153: BF 00 A0 F8  LDA $F8A000,X     long,X
$00157: 2C B0 D0     BIT $D0B0
$0015A: 70 04        BVS $0160
$0015C: BF 00 A0 F9  LDA $F9A000,X     long,X
$00160: DF 00 A0 01  CMP $01A000,X     long,X -- bank 1 $A000, the BASIC shadow
$00164: F0 03        BEQ $0169
$00166: 4C 51 07     JMP $0751         mismatch -> elsewhere
$00169: E8           INX
$0016A: D0 E7        BNE $0153
```

It verifies 256 bytes of the bank-1 BASIC shadow against the ROM's own BASIC
image, choosing `$F8A000` or `$F9A000` on `$D0B0` bit 6. That is the shape of
the checks the startup error codes describe, and the next thing to follow is
`JSR $0574`.

## The exact instruction where we diverge

Decoded with `Tools/disasm816.py`, which parses this repository's own validated
65816 opcode table rather than a hand-written one. That matters: the boot uses
long addressing (`$BF` `LDA long,X`, `$DF` `CMP long,X`, `$7F` `ADC long,X`) and
`REP`/`SEP` width changes, and a 6502-shaped table desynchronises on the first
one and prints garbage thereafter.

The accelerator's boot, in order:

* `$0574` — **ROM checksum.** Native mode, 16-bit X, sums ROM bank `$F8` from
  offset 4 upward counting carries in Y, then compares the 16-bit result with
  the word stored at `$F80000`. Verified by hand against the image: computed
  `$ED9B` vs stored `$ED9B`, and `$4EF7` vs `$4EF7` for bank `$F9`. **Our
  emulator passes this** — the `BNE` at `$0179` is not taken.

* `$01F8` — **the bank-1 window loads, done by MVN block moves:**

  ```
  $00210: MVN $01,$F8    8KB  ROM $F8:A000 -> bank1 $A000   BASIC
  $0021C: MVN $01,$F8    8KB  ROM $F8:E000 -> bank1 $E000   KERNAL
  $00228: MVN $01,$F8    8KB  ROM $F8:8000 -> bank1 $6000   ALT KERNAL
  ```

  Note the third: the alternate KERNAL is copied from ROM **`$8000`**, not ROM
  `$6000`. The boot loads these itself, so seeding them is belt-and-braces
  rather than required.

* `$0314` — **the RAMLink gate**, and where it ends:

  ```
  $00314: LDA $D000
  $00317: BIT $D0B0
  $0031A: BVC $0325
  $0031C: STA $D0BE      DOS extension ON
  $0031F: LDA $E0AB      <-- read WITH the extension enabled
  $00322: STA $D0BF      DOS extension OFF
  $00325: CMP #$7E       <-- must be $7E
  $00327: BNE $038F      <-- we take this, skipping everything below
  $00329: LDA #$C0
  $0032B: STA $D200
  $0032E: STA $DF7E      RAMLink ON -- the init VICE performs and we never reach
  ```

**So the whole RAMLink path hinges on one byte: `$E0AB` read while the CPU DOS
extension is enabled must return `$7E`.** Neither ROM `$E0AB` (`$C8`) nor ROM
`$80AB` (`$F6`, which is what lands in bank1 `$60AB`) is `$7E`, so the value
cannot be coming from either window as we map them.

It comes from the RAMLink's own EPROM: `ramlink201.bin[$A0AB]` is `$7E`. The
probe is a presence check, and the DOS extension is what makes the cartridge
visible to it.

### What the DOS extension really does

`mem_pla_config_changed()` folds the DOS-extension bit into the memory
configuration index itself:

```c
mem_config = ((mem_pport & 7) | (export.exrom << 3) | (export.game << 4)
            | (mem_reg_hwenable << 5) | (mem_reg_dosext << 6)
            | (mem_reg_bootmap << 7));
```

so enabling it reselects the whole read table. Diffing every entry of VICE's
`config[AREAS][256]` against the same entry with bit 6 set gives the complete
rule:

| When | What the extension does |
|---|---|
| bootmap **or** hwenable clear | `$1000-$5FFF` and `$8000-$9FFF` map bank 1. Nothing else changes. |
| bootmap **and** hwenable set, cartridge holding Ultimax (`game=1, exrom=0`) | the accelerator's EPROM is dropped and `$1000-$CFFF` and `$E000-$FFFF` become an Ultimax view of the expansion port |
| bootmap **and** hwenable set, no such cartridge | assorted `RC`/`RL`/`CR` changes; `$E000-$FFFF` stays on the EPROM |

The middle row is the boot-time configuration with a RAMLink attached, and it
is the only way `ramlink_romh_read()` is ever reached on an SCPU64.

**An earlier version of this document claimed the opposite** — that
`$e000-ffff` changed "in only 8 ultimax corners, therefore irrelevant". The
count was right and the conclusion was wrong: those 8 entries *are* the
configuration CMD's boot code runs in.

What the overlay amounts to was measured out of a running VICE rather than
derived through its cartridge slot dispatch. At `cfg=$f7` with an 8MB RAMLink:

```
$1234 -> ram0      $6234 -> ram0      $8234 -> ram0
$a234 -> bank1     $c234 -> ram0      $e0ab -> $7e
```

which is simply: stop mapping the boot ROM, use the ordinary map, and let the
cartridge answer `$E000-$FFFF`. Writes need nothing special — with a RAMLink
fitted every Ultimax store in VICE ends in `mem_store_without_ultimax()`, which
is RAM.

### Two further rules the same table settles

**The bank-1 windows are read-only.** VICE's `R1` entry installs a read
function and a read base and leaves the write hook at its preset — `ram_store`,
bank 0 — with the comment `/* write hook preset, ram */`. Routing writes to
bank 1 as well corrupts RL-DOS, which lives in bank 1 `$8000-$9FFF` while CMD's
DOS writes that range as ordinary RAM.

**The cartridge KERNAL is never banked in on an accelerated machine**, outside
the overlay. No configuration with the boot ROM unmapped has an `RH` or `UM`
entry for `$E000-$FFFF`. This is not a policy choice: CMD's ALT KERNAL runs

```
$FE70  STA $DF7E     RAMLink ON
$FE73  JSR $FE98
$FE76  STA $DF7F     RAMLink OFF
```

and if switching RAMLink on swapped its ROM in underneath the program counter,
the `JSR` at `$FE73` would be fetched from the cartridge instead.
