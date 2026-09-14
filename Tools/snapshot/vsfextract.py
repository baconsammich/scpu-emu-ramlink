# Extract the CPU-view (SRAM bank 0) and board-DRAM 64K images from a vsf.
#
#     vsfextract.py <snapshot.vsf> [output-directory]
#
# The snapshot is NOT in this repository -- a VICE .vsf captures the whole
# machine, ROM images included, so it is not redistributable. Take your own in
# VICE SCPU64 at the moment you are investigating and pass it in.
import os, sys

if len(sys.argv) < 2:
    sys.exit(__doc__ or 'usage: vsfextract.py <snapshot.vsf> [output-directory]')
src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(src))

def u32(b, o): return int.from_bytes(b[o:o+4], 'little')
data = open(src, 'rb').read()
off = 37
if data[off:off+13] == b'VICE Version\x1a':
    off += 21
while off + 22 <= len(data):
    name = data[off:off+16].rstrip(b'\x00').decode(errors='replace')
    size = u32(data, off+18)
    if name == 'C64MEM':
        body = data[off+22+52 : off+22+52+196608]
        open(os.path.join(out, 'snap_ram_cpu.bin'), 'wb').write(body[65536:131072])
        open(os.path.join(out, 'snap_ram_board.bin'), 'wb').write(body[0:65536])
        print('wrote snap_ram_cpu.bin (SRAM bank0) + snap_ram_board.bin (board DRAM) to', out)
        break
    off += size
