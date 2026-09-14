# VICE as a reference oracle

A headless VICE `xscpu64` instrumented to log PC-tagged cartridge I/O. Diffing
its access sequence against `Tools/viceconf/scpu_trace`'s is the method that
found every RAMLink bug in 00.05.00-RL and 00.06.00-RL. Each round, the first
mismatch named the next bug.

These patches are kept because rebuilding the instrumentation from scratch is
most of an afternoon, and because the method is worth repeating.

## Build

```sh
tar xzf vice-3.10.tar.gz
cd vice-3.10
patch -p1 < .../Tools/vice-oracle/scpu64mem-trace.patch
patch -p1 < .../Tools/vice-oracle/ramlink-trace.patch
./configure --enable-headlessui --disable-pdf-docs --without-oss --without-pulse
make -j8
```

On macOS the configure step wants a few tools that are easy to shim; the build
needs no GUI toolkit at all with `--enable-headlessui`.

## Run

```sh
./src/xscpu64 -directory ./data \
  -scpu64 scpu-dos-2.04.bin \
  -ramlinkbios ramlink201.bin -ramlinksize 8 \
  -ramlinkimage card.rl -ramlinkimagerw -ramlink \
  -warp -limitcycles 30000000 > trace.log 2>&1
```

Two argument-order traps, both of which cost time to find:

- **`-ramlink` must come AFTER `-ramlinkbios`.** Setting the resource attaches
  the BIOS, so the other order is rejected.
- **`-keybuf` needs lowercase.** A C64 renders typed uppercase as PETSCII
  graphics.

## What the patches log

`SCPUTRACE CART` lines from `scpu64io_de00/df00_read/store` — every access to
`$DE00-$DFFF` with the PC that caused it. That is the same shape
`scpu_trace --cartlog N` prints, so the two diff directly:

```sh
grep -a "SCPUTRACE CART" trace.log \
  | sed -E 's/.*CART ([RW]) \$([0-9a-f]+) = ([0-9a-f]+).*/\1 \2 \3/' > vice.seq
```

Diff `op+address` separately from `op+address+value`. A value-only difference
is usually power-on state or open bus; an **address** difference is control
flow, and that is the one that matters.

The patches also carry the probes that answered specific questions along the
way — the per-area handler identity at a given `mem_config`, and what the
installed handler actually returns for `$E0AB`.

## The lesson that justifies keeping this

Reading VICE's `config[AREAS][256]` table and reasoning about it produced a
confident, wrong conclusion: that the DOS extension leaves `$E000-$FFFF` alone
because only 8 entries change it and those are "ultimax corners". The count was
right. Those 8 entries are the boot-time configuration, and they are the whole
RAMLink detection path.

Measuring what the installed handler actually returned took ten minutes and did
not lie. When the question is "what does the hardware do here", probe the
running emulator; do not read its source and infer.
