#!/usr/bin/env python3
"""
Disassemble a 65816 image using THIS repository's own opcode table.

Hand-written length tables kept mis-decoding CMD's ROM -- 65816 long addressing
($BF LDA long,X, $DF CMP long,X) reads as garbage under a 6502 table, and a
wrong length desynchronises everything after it. Source/CPU/W65C816 already has
a table that agrees with 5,080,000 SingleStepTests vectors, so parse that
instead of writing a fourth one.

  disasm816.py <image> <start-hex> <end-hex> [--base HEX] [--m8|--m16] [--x8|--x16]

--base is the address the image's byte 0 corresponds to (default: same as start,
i.e. the offset IS the address). Widths default to 8-bit, which is emulation
mode and what CMD's boot runs in.
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load_table():
    src = open(os.path.join(ROOT, 'Source/CPU/W65C816/w65c816_opcodes.cpp')).read()
    ops = re.findall(r'\{\s*"(\w+)",\s*(W65M_\w+),\s*(\d+),.*?\}\s*,\s*//\s*\$([0-9A-Fa-f]{2})', src)
    table = {}
    for mnem, mode, _cyc, hx in ops:
        table[int(hx, 16)] = (mnem, mode)
    hdr = open(os.path.join(ROOT, 'Source/CPU/W65C816/w65c816_opcodes.h')).read()
    blk = hdr[hdr.index('W65M_IMP\n'):]
    lens = {}
    for m in re.finditer(r'(\d+),\s*//\s*(W65M_\w+)', hdr):
        lens[m.group(2)] = int(m.group(1))
    return table, lens

TBL, LEN = load_table()

FMT = {
 'W65M_IMP':  lambda a,o,b: '',
 'W65M_ACC':  lambda a,o,b: ' A',
 'W65M_IMM8': lambda a,o,b: ' #$%02X' % o[0],
 'W65M_IMMM': lambda a,o,b: ' #$%02X' % o[0] if len(o)==1 else ' #$%02X%02X' % (o[1],o[0]),
 'W65M_IMMX': lambda a,o,b: ' #$%02X' % o[0] if len(o)==1 else ' #$%02X%02X' % (o[1],o[0]),
 'W65M_ZP':   lambda a,o,b: ' $%02X' % o[0],
 'W65M_ZPX':  lambda a,o,b: ' $%02X,X' % o[0],
 'W65M_ZPY':  lambda a,o,b: ' $%02X,Y' % o[0],
 'W65M_IZX':  lambda a,o,b: ' ($%02X,X)' % o[0],
 'W65M_IZY':  lambda a,o,b: ' ($%02X),Y' % o[0],
 'W65M_IZP':  lambda a,o,b: ' ($%02X)' % o[0],
 'W65M_IZL':  lambda a,o,b: ' [$%02X]' % o[0],
 'W65M_IZLY': lambda a,o,b: ' [$%02X],Y' % o[0],
 'W65M_ABS':  lambda a,o,b: ' $%02X%02X' % (o[1],o[0]),
 'W65M_ABX':  lambda a,o,b: ' $%02X%02X,X' % (o[1],o[0]),
 'W65M_ABY':  lambda a,o,b: ' $%02X%02X,Y' % (o[1],o[0]),
 'W65M_ABL':  lambda a,o,b: ' $%02X%02X%02X' % (o[2],o[1],o[0]),
 'W65M_ABLX': lambda a,o,b: ' $%02X%02X%02X,X' % (o[2],o[1],o[0]),
 'W65M_IND':  lambda a,o,b: ' ($%02X%02X)' % (o[1],o[0]),
 'W65M_IAX':  lambda a,o,b: ' ($%02X%02X,X)' % (o[1],o[0]),
 'W65M_INDL': lambda a,o,b: ' [$%02X%02X]' % (o[1],o[0]),
 'W65M_SR':   lambda a,o,b: ' $%02X,S' % o[0],
 'W65M_SRY':  lambda a,o,b: ' ($%02X,S),Y' % o[0],
 'W65M_REL':  lambda a,o,b: ' $%04X' % ((a+2+(o[0]-256 if o[0]>127 else o[0])) & 0xFFFF),
 'W65M_RELL': lambda a,o,b: ' $%04X' % ((a+3+((o[1]<<8|o[0])-65536 if (o[1]<<8|o[0])>32767 else (o[1]<<8|o[0]))) & 0xFFFF),
 'W65M_BM':   lambda a,o,b: ' $%02X,$%02X' % (o[0],o[1]),
}

def disasm(img, start, end, base, m16, x16):
    i = start
    while i < end:
        op = img[i - base]
        mnem, mode = TBL.get(op, ('???', 'W65M_IMP'))
        n = LEN.get(mode, 1)
        if mode == 'W65M_IMMM' and m16: n += 1
        if mode == 'W65M_IMMX' and x16: n += 1
        ops = img[i - base + 1 : i - base + n]
        try:    txt = mnem + FMT[mode](i, list(ops), base)
        except Exception: txt = mnem + ' ?'
        raw = ' '.join('%02X' % img[i - base + k] for k in range(n))
        print('  $%05X: %-12s %s' % (i, raw, txt))
        i += n

if __name__ == '__main__':
    a = sys.argv[1:]
    img = open(a[0], 'rb').read()
    start = int(a[1], 16); end = int(a[2], 16)
    base = 0
    m16 = '--m16' in a; x16 = '--x16' in a
    if '--base' in a: base = int(a[a.index('--base')+1], 16)
    disasm(img, start, end, base, m16, x16)
