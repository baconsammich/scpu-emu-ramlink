#!/usr/bin/env bash
#
# SCPU-EMU - assemble a release.
#
#   Tools/mkrelease.sh [destination]
#
# Produces, under <destination>/scpu-emu-<version>/ :
#
#   SDCard/                        ready to copy onto a FAT32 card
#   scpu-emu-<version>-sdcard.zip  the same, zipped
#   scpu-emu-<version>-src.tar.gz  source only -- no ROMs, no build output
#   RELEASE-NOTES.md
#   reports/                       what was actually run, and its output
#
# Default destination is ~/Desktop.
#
# The SD card payload contains the four ROM images staged from ROMs/. Those are
# Commodore's and CMD's property: the zip is for the machine it was built on,
# and is not redistributable. The source tarball deliberately excludes them.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$HOME/Desktop}"

VERSION="$(sed -n 's/.*SCPU_EMULATOR_BUILD "\(.*\)".*/\1/p' "$REPO/Source/Common/version.h")"
[ -n "$VERSION" ] || { echo "ERROR: cannot read version from Source/Common/version.h"; exit 1; }

NAME="scpu-emu-$VERSION"
OUT="$DEST/$NAME"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

say "Building $NAME into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/reports"

# --- gates ------------------------------------------------------------------
# A release that was never tested is not a release. Both of these must pass;
# `set -e` means a failure stops here rather than shipping.
say "Host test suite"
( cd "$REPO" && make tests ) 2>&1 | tee "$OUT/reports/tests.txt" | tail -3

say "Sanitizers (ASan + UBSan)"
( cd "$REPO" && make sanitizers ) 2>&1 | tee "$OUT/reports/sanitizers.txt" | tail -3

# --- payload ----------------------------------------------------------------
say "Staging SD card"
( cd "$REPO" && make sdcard ) 2>&1 | tee "$OUT/reports/sdcard.txt" | tail -20

[ -f "$REPO/SDCard/kernel8.img" ] \
	|| { echo "ERROR: SDCard/kernel8.img missing -- run 'make firmware' first"; exit 1; }

cp -R "$REPO/SDCard" "$OUT/SDCard"

say "Packaging"
( cd "$OUT" && zip -qr "$NAME-sdcard.zip" SDCard )

# A second card payload with every ROM removed.
#
# The full zip above is not redistributable: it carries Commodore's BASIC,
# KERNAL and character ROMs, CMD's SuperCPU DOS and RAMLink EPROM, and a
# RAMCard image, all staged from whatever is in this machine's ROMs/. This one
# is everything else -- the firmware, the Pi boot files, and the config -- so
# it can be handed to someone who has their own ROMs.
#
# It is a SEPARATE artifact rather than the default because a card built from
# it does not boot until the ROMs are added, and silently shipping a
# non-booting card would be the worse failure.
say "Packaging (ROM-free)"
rm -rf "$OUT/SDCard-romfree"
cp -R "$OUT/SDCard" "$OUT/SDCard-romfree"
rm -f "$OUT/SDCard-romfree/SCPU/"*.rom \
      "$OUT/SDCard-romfree/SCPU/"*.img \
      "$OUT/SDCard-romfree/SCPU/"*.bin \
      "$OUT/SDCard-romfree/SCPU/"*.rl

cat > "$OUT/SDCard-romfree/SCPU/PUT-ROMS-HERE.txt" <<'ROMDOC'
This card is deliberately incomplete. It will NOT boot as it stands.

SCPU-EMU ships no ROMs and never will: they are Commodore's and CMD's
property, not the project's to give away. Supply your own, dumped from
hardware you own or obtained from a source you are entitled to use, and put
them in THIS directory:

  basic.rom     8192 bytes   Commodore BASIC V2         (901226-01)
  kernal.rom    8192 bytes   Commodore KERNAL           (901227-03)
  chargen.rom   4096 bytes   Commodore character ROM    (906143-02)
  scpu.rom    131072 bytes   CMD SuperCPU DOS 2.04

Optional, and only meaningful with RAMLINK_SIZE set in scpu.cfg:

  ramlink.rom  65536 bytes   CMD RAMLink EPROM
  ramlink.img      any size  a RAMCard image, no larger than the fitted card

If the first four are not all present the RAD does not take the bus at all and
your Commodore boots normally, which is the intended failure: a machine that
will not start is worse than one that starts without the accelerator.

WHERE TO GET THEM

  The three Commodore ROMs ship with VICE, which is the easiest route and
  needs no dumping hardware. Install VICE (https://vice-emu.sourceforge.io/)
  and copy from its data directory:

    <vice>/data/C64/basic-901226-01.bin    -> basic.rom     (8192 bytes)
    <vice>/data/C64/kernal-901227-03.bin   -> kernal.rom    (8192 bytes)
    <vice>/data/C64/chargen-906143-02.bin  -> chargen.rom   (4096 bytes)

  On macOS that is inside the app bundle; on Linux usually
  /usr/share/vice/C64; on Windows, C64\ beside the executables.

  Dumping from hardware you own always works and gives you the exact
  revisions your machine has.

  The SuperCPU DOS ROM is a free download from Zimmers, the long-running CBM
  archive:

    https://www.zimmers.net/anonftp/pub/cbm/firmware/misc/cmd/
      scpu-dos-2.04.bin   131072 bytes  -> scpu.rom
      scpu-dos-1.4.bin    131072 bytes     the documented fallback

  The RAMLink EPROM and a RAMCard image are NOT in that directory. CMD is long
  gone and the EPROM was never sold separately. One archive that has been used
  for both:

    https://www.mediafire.com/file/empvwwbu2oquuct/ramlink.zip  (334 KB)

  That is a file-locker upload, not a curated archive: nobody vouches for what
  is in it and the link can stop working. Check what you get --

    ramlink201.bin  65536 bytes  sha1 01e2f634c7464e5749c464b59f4c590903a01bd5
    ramlink.rl    8388608 bytes  sha1 a136863c39c0974d246099af755c8689b5494c4f

  -- those being the images this emulator was tested against. Dumping from a
  RAMLink you own remains the route with no questions attached.

  Commodore ROMs are also mirrored on archive.org, zimmers.net and the C64
  preservation sites. Whether downloading them is lawful for you depends on
  your jurisdiction and on whether you own the hardware.

A RAMCard image must MATCH the card RAMLINK_SIZE fits, not merely fit inside
it: RAMLink keeps its system partition at the TOP of the card, so a padded
image leaves that structure stranded in the middle and the card reads as
unformatted. Tools/mkramcard.py in the source tarball resizes one correctly.
ROMDOC

( cd "$OUT" && zip -qr "$NAME-sdcard-romfree.zip" SDCard-romfree )

# Source tarball: everything tracked-ish, minus ROMs, build output and the
# toolchain. --exclude patterns are matched against the path inside the archive.
tar -czf "$OUT/$NAME-src.tar.gz" \
	-C "$(dirname "$REPO")" \
	--exclude="$(basename "$REPO")/ROMs/*.bin" \
	--exclude="$(basename "$REPO")/ROMs/*.rom" \
	--exclude="$(basename "$REPO")/ROMs/*.rl" \
	--exclude="$(basename "$REPO")/ROMs/*.img" \
	--exclude="$(basename "$REPO")/.git" \
	--exclude="$(basename "$REPO")/ROMs/*.d81" \
	--exclude="$(basename "$REPO")/build" \
	--exclude="$(basename "$REPO")/_toolchain" \
	--exclude="$(basename "$REPO")/SDCard" \
	--exclude="$(basename "$REPO")/Firmware/RaspberryPi/start*.elf" \
	--exclude="$(basename "$REPO")/Firmware/RaspberryPi/fixup*.dat" \
	--exclude="$(basename "$REPO")/Firmware/RaspberryPi/bootcode.bin" \
	--exclude="$(basename "$REPO")/.git" \
	--exclude=".DS_Store" \
	"$(basename "$REPO")"

# Release notes live in the repo next to the code that produced them.
if [ -f "$REPO/Docs/RELEASE-NOTES.md" ]; then
	cp "$REPO/Docs/RELEASE-NOTES.md" "$OUT/RELEASE-NOTES.md"
fi

# --- manifest ---------------------------------------------------------------
{
	echo "# $NAME - contents"
	echo
	echo "Built $(date -u '+%Y-%m-%d %H:%M UTC') on $(uname -srm)"
	echo
	echo '## SD card (full -- contains ROMs, not redistributable)'
	echo
	echo '| file | bytes | sha1 |'
	echo '|---|---:|---|'
	( cd "$OUT/SDCard" && find . -type f | sort | while read -r f; do
		printf '| `%s` | %s | `%s` |\n' "${f#./}" "$(wc -c < "$f" | tr -d ' ')" "$(shasum "$f" | cut -d' ' -f1)"
	done )
	echo
	echo '## SD card (ROM-free -- safe to share)'
	echo
	echo '| file | bytes | sha1 |'
	echo '|---|---:|---|'
	( cd "$OUT/SDCard-romfree" && find . -type f | sort | while read -r f; do
		printf '| `%s` | %s | `%s` |\n' "${f#./}" "$(wc -c < "$f" | tr -d ' ')" "$(shasum "$f" | cut -d' ' -f1)"
	done )
} > "$OUT/MANIFEST.md"

say "Done"
echo "  $OUT"
( cd "$OUT" && find . -maxdepth 1 -type f | sort | sed 's|^\./|    |' )
