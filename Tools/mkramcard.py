#!/usr/bin/env python3
"""
Resize a CMD RAMLink RAMCard image.

    Tools/mkramcard.py <in.rl> <out.rl> <size-in-MB>

A RAMCard image is a raw dump of the card's RAM, and it CANNOT simply be
padded to a larger size. The system partition -- which holds the device
number, the disk name and the partition table, and without which RAMLink has
no configuration at all -- lives at the TOP of the card, at (size - $1000).
Pad an 8MB image out to 16MB and that structure ends up in the middle, where
RAMLink does not look: the card then reads as unformatted.

Measured, with CMD's own DOS booting against each:

    8MB card,  8MB image                 system partition found, 0 bytes changed
    16MB card, 8MB image padded          nothing found, 333 bytes changed
    16MB card, image through this tool   system partition found, 0 bytes changed

So this moves the system partition to the new top of card and rewrites the
SYSTEM entry's start address to match. Every other partition keeps its
address, which stays valid because the card only grew underneath them.

Partition entry layout, confirmed against a configured image:

    0-1    flags            2   type ($FF system, $01 native, $02 1541, $04 1581)
    5-20   name, $A0-padded     21-24  start address, big-endian byte offset

The start-address reading is not a guess. In ramlink.rl the entries land at
$000000 C64OS, $500000 1541-1, $52AB00 1541-2, $555600 1581-1, $61D600
1581-2 -- and $52AB00-$500000 is $2AB00, exactly one 1541 disk (174848
bytes), while $61D600-$555600 is $C8000, exactly one 1581 (819200 bytes).
"""
import sys

SYS_OFFSET   = 0x1000          # system partition sits this far below the top
ENTRY_TABLE  = 0x800           # ...and the partition table this far into it
ENTRY_SIZE   = 32
START_FIELD  = 21
TYPE_SYSTEM  = 0xFF


def system_partition_ok(image, size):
    """Does `image` carry a plausible system partition for a card of `size`?"""
    sys_at = size - SYS_OFFSET
    if sys_at + ENTRY_TABLE + ENTRY_SIZE > len(image):
        return False, None
    entry = image[sys_at + ENTRY_TABLE : sys_at + ENTRY_TABLE + ENTRY_SIZE]
    if entry[2] != TYPE_SYSTEM:
        return False, None
    name = bytes(entry[5:21]).replace(b'\xa0', b' ').decode('latin1').strip(' \x00')
    if name != 'SYSTEM':
        return False, None
    return True, sys_at


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    src_path, dst_path, mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if mb not in (1, 2, 4, 8, 16):
        sys.exit('size must be one of 1, 2, 4, 8, 16 MB -- RAMLINK_SIZE has no '
                 'selector for anything else')

    src = bytearray(open(src_path, 'rb').read())
    old_size, new_size = len(src), mb << 20

    ok, old_sys = system_partition_ok(src, old_size)
    if not ok:
        sys.exit(f'{src_path} has no SYSTEM partition at ${old_size - SYS_OFFSET:06X}. '
                 'It is not a configured RAMCard image, so there is nothing to '
                 'relocate -- format a blank card from RAMLink\'s own tools instead.')

    if new_size == old_size:
        sys.exit(f'{src_path} is already {mb}MB')
    if new_size < old_size:
        # Shrinking would drop whatever lives above the new top, and the
        # partition table cannot say what is safe to lose. Refuse rather than
        # silently truncate someone's partitions.
        sys.exit(f'refusing to shrink {old_size >> 20}MB to {mb}MB: partitions '
                 'above the new top would be lost, and this tool cannot tell '
                 'which of them you still want')

    dst = src + bytearray(new_size - old_size)
    new_sys = new_size - SYS_OFFSET

    dst[new_sys : new_sys + SYS_OFFSET] = dst[old_sys : old_sys + SYS_OFFSET]

    # Point the SYSTEM entry at where SYSTEM now actually is. Boot works
    # without this -- CMD's DOS locates the partition by card size, not by
    # following the pointer -- but leaving it stale would describe a layout
    # that is not the one on the card, and RAMLink's own partition editor has
    # no reason to be as forgiving.
    ent = new_sys + ENTRY_TABLE + START_FIELD
    dst[ent : ent + 4] = new_sys.to_bytes(4, 'big')

    # The old copy is left where it was. Removing it is a third guess with no
    # evidence behind it, and it sits inside space the partition table already
    # accounts for.

    open(dst_path, 'wb').write(bytes(dst))

    print(f'{src_path} ({old_size >> 20}MB) -> {dst_path} ({mb}MB)')
    print(f'  system partition ${old_sys:06X} -> ${new_sys:06X}')
    table = new_sys + ENTRY_TABLE
    print('  partitions carried over:')
    for i in range(16):
        e = dst[table + i * ENTRY_SIZE : table + (i + 1) * ENTRY_SIZE]
        if len(e) < ENTRY_SIZE or not any(e):
            continue
        name = bytes(e[5:21]).replace(b'\xa0', b' ').decode('latin1').strip(' \x00')
        if not name:
            continue            # a slot with stray bytes but no partition in it
        start = int.from_bytes(e[START_FIELD:START_FIELD + 4], 'big')
        print(f'    {name:<12} type ${e[2]:02X}  at ${start:06X}')


if __name__ == '__main__':
    main()
