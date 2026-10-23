#!/usr/bin/env python3
"""Check the native PIE's moved data and code-offset table destinations."""
import importlib.util
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('asm64_prepare', ROOT / 'ps5/tools/asm64_prepare.py')
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)
source = (ROOT / 'ASM_Sources/bedlam2.asm').read_text(errors='replace')
code, data, moved = converter.extract_embedded_data(source)
assert len(moved) == 128 and sum(name.startswith('jpt_') for name in moved) == 117
assert re.search(r'^___begtext\s+db\b', code, re.M)
assert re.search(r'^word_6D1C2\s+dw\b', code, re.M)
# Bare bytes are often real opcodes or prefixes, not data islands.
assert 'db 0E9h' in code and re.search(r'^\s*db\s+2Eh\b', code, re.M)

image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/ps5-frontend/bedlam2_native_pie.elf'
image = image_path.read_bytes()
assert image[:6] == b'\x7fELF\x02\x01'
section_offset = struct.unpack_from('<Q', image, 40)[0]
section_size, section_count = struct.unpack_from('<HH', image, 58)
sections = [struct.unpack_from('<IIQQQQIIQQ', image, section_offset + index * section_size)
            for index in range(section_count)]
symbols = {}
for section in sections:
    if section[1] != 2:  # SHT_SYMTAB
        continue
    strings = sections[section[6]]
    names = image[strings[4]:strings[4] + strings[5]]
    for offset in range(section[4], section[4] + section[5], section[9]):
        name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', image, offset)
        if name and index < len(sections):
            text = names[name:names.index(0, name)].decode()
            symbols[text.lower()] = (value, index)

def contents(name, size):
    value, index = symbols[name.lower()]
    section = sections[index]
    offset = section[4] + value - section[3]
    assert section[4] <= offset and offset + size <= section[4] + section[5]
    return image[offset:offset + size]

begin = symbols['bedlam2_data_base'][0]
end = symbols['bedlam2_data_end'][0]
text_begin = symbols['bedlam2_text_base'][0]
for name in moved:
    value, index = symbols[name]
    assert begin <= value < end, name
    assert sections[index][2] & 2 and not sections[index][2] & 4, name  # ALLOC, not EXEC
for name in ('___begtext', 'word_6d1c2'):
    # JWasm omits unexported local labels; source checks above retain those
    # opcode declarations. Check section flags for labels kept in the PIE.
    if name in symbols:
        assert sections[symbols[name][1]][2] & 4, name

active = None
tables = {}
for line in data:
    statement = line.partition(';')[0]
    declaration = re.match(r'^\s*([A-Za-z_.$?@][\w.$?@]*)\s+(db|dw|dd|dq)\b', statement, re.I)
    if declaration:
        name = declaration[1].lower()
        active = name if name.startswith('jpt_') or name in {'off_6e6a5', 'off_705e9'} else None
    if active:
        tables.setdefault(active, []).extend(re.findall(r'\boffset\s+([A-Za-z_.$?@][\w.$?@]*)', statement, re.I))
entries = 0
for table, targets in tables.items():
    values = struct.unpack('<' + 'I' * len(targets), contents(table, 4 * len(targets)))
    for stored, target in zip(values, targets):
        assert stored == (symbols[target.lower()][0] - text_begin) & 0xffffffff, (table, target)
        assert sections[symbols[target.lower()][1]][2] & 4, target
        entries += 1
assert len(tables) == 119 and entries == 1340
expected = b'\x78' + struct.pack('<7I', 0x27282D61, 0x21222325, 0x17191A20,
                               0x13141516, 0x0B0C0D0E, 0x0708090A, 0x02030405) + b'\x01\x00'
assert contents('byte_14723', len(expected)) == expected
# The 32-byte startup copy includes the first byte of the adjacent default
# jump-table entry, just as before extraction. Preserve that adjacency.
assert symbols['jpt_147f6'][0] == symbols['byte_14723'][0] + len(expected)
generated_code = (ROOT / 'build/ps5-frontend/bedlam2-engine-x64/bedlam2.asm').read_text()
for line in generated_code.splitlines():
    statement = line.partition(';')[0]
    if 'bedlam2_text_base' in statement:
        names = {name.lower() for name in re.findall(r'[A-Za-z_.$?@][\w.$?@]*', statement)}
        assert not names.intersection(moved), statement
print(f'Engine data checks passed: {len(moved)} readable symbols, 117 switch tables, '
      f'2 codec tables, {entries} valid code offsets, lookup bytes and opcode blocks preserved.')
