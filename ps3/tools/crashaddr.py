#!/usr/bin/env python3
#
# crashaddr.py -- names the addresses of a debug package's crash dump.
#
#   python3 ps3/tools/crashaddr.py CrispyStrife-1.3-debug.elf crispy_log.txt
#
# The [stack] and [ftrace] lines hold function descriptors (the PPU's
# 64-bit ELF calls functions through .opd entries, and that is what
# -finstrument-functions hands over); the @xxxxxxxx of the [crumb] lines
# are return addresses in the code. Both are looked up in the ELF of the
# same build: its symbol table and .opd, read here with no other tool.
# Lines without addresses are printed unchanged.
#

import re
import struct
import sys
from bisect import bisect_right


def read_elf(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 2:
        sys.exit('%s: not a 64-bit big-endian ELF' % path)

    (shoff,) = struct.unpack_from('>Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('>HHH', data, 0x3a)

    sections = []
    for i in range(shnum):
        (name, stype, flags, addr, offset, size, link, info, align,
         entsize) = struct.unpack_from('>IIQQQQIIQQ', data, shoff + i * shentsize)
        sections.append(dict(name=name, type=stype, addr=addr, offset=offset,
                             size=size, link=link, entsize=entsize))
    strtab = sections[shstrndx]

    def cstr(off):
        end = data.index(b'\0', off)
        return data[off:end].decode('latin-1')

    for s in sections:
        s['name'] = cstr(strtab['offset'] + s['name'])
    return data, sections, cstr


def build_tables(path):
    data, sections, cstr = read_elf(path)
    opd = next((s for s in sections if s['name'] == '.opd'), None)
    symtab = next((s for s in sections if s['type'] == 2), None)  # SHT_SYMTAB
    if symtab is None:
        sys.exit('%s: no symbol table (stripped?)' % path)
    strs = sections[symtab['link']]

    desc = {}       # descriptor address -> name
    code = {}       # code address -> name
    for i in range(symtab['size'] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('>IBBHQQ', data, symtab['offset'] + i * 24)
        if st_info & 0xf != 2 or st_value == 0:     # STT_FUNC
            continue
        name = cstr(strs['offset'] + st_name)
        if opd and opd['addr'] <= st_value < opd['addr'] + opd['size']:
            desc[st_value] = name
            (entry,) = struct.unpack_from(
                '>Q', data, opd['offset'] + st_value - opd['addr'])
            code.setdefault(entry, name)
        else:
            code.setdefault(st_value, name.lstrip('.'))

    starts = sorted(code)
    return desc, code, starts


def name_of(addr, desc, code, starts):
    if addr in desc:
        return desc[addr]
    i = bisect_right(starts, addr) - 1
    if i >= 0 and addr - starts[i] < 0x10000:
        return '%s+0x%x' % (code[starts[i]], addr - starts[i])
    return '?'


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: crashaddr.py <package .elf> <crispy_log.txt>')
    desc, code, starts = build_tables(sys.argv[1])

    with open(sys.argv[2], errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('[stack] ') and re.match(r'\[stack\] \d+ [0-9a-f]{8}$', line):
                n, a = line.split()[1:3]
                print('[stack] %3s %s' % (n, name_of(int(a, 16), desc, code, starts)))
            elif line.startswith('[ftrace]'):
                words = []
                for w in line.split()[1:]:
                    v = int(w, 16)
                    words.append(('<' if v & 1 else '>') +
                                 name_of(v & ~1, desc, code, starts))
                print('[ftrace] ' + ' '.join(words))
            elif line.startswith('[crumb]') and '@' in line:
                head, a = line.rsplit('@', 1)
                print('%s@%s' % (head, name_of(int(a, 16), desc, code, starts)))
            else:
                print(line)


if __name__ == '__main__':
    main()
