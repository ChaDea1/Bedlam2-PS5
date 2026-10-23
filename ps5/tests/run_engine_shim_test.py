#!/usr/bin/env python3
"""Build/test the real native stack entry and external-call assembly on Linux."""
from pathlib import Path
import importlib.util
import re
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
output = root / 'build/host-checks/engine-shim'
output.mkdir(parents=True, exist_ok=True)
subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-fsanitize=undefined',
                '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
                '-Ips5', '-c', str(root / 'ps5/native_file_bridge.cpp'),
                '-o', str(output / 'files.o')], cwd=root, check=True)
provided = {line.split()[-1] for line in subprocess.check_output(
    ['nm', '-g', '--defined-only', str(output / 'files.o')], text=True).splitlines()}
subprocess.run(['cc', '-c', str(root / 'ps5/native_external_thunks.S'), '-o', str(output / 'thunks.o')], check=True)
undefined = subprocess.check_output(['nm', '-u', str(output / 'thunks.o')], text=True)
# Exercise the production directory bridge. Trap in unrelated application
# functions so an accidental extra call cannot silently succeed.
libc = {'exit', 'getcwd', 'mkdir', 'printf', 'putchar', 'puts', '_GLOBAL_OFFSET_TABLE_'}
stubs = ['.text']
for line in undefined.splitlines():
    name = line.split()[-1]
    if name not in libc | provided | {'bedlam2_ps5_malloc', 'bedlam2_ps5_free'}:
        stubs += [f'.globl {name}', f'{name}:', 'ud2']
stubs += ['.section .note.GNU-stack,"",@progbits']
(output / 'stubs.S').write_text('\n'.join(stubs) + '\n')
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
command = ['g++', '-std=c++17', '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all',
           '-ffunction-sections', '-fdata-sections', '-DBEDLAM2_PS5_NATIVE=1',
           '-Ips5', '-ICPP_Sources', *flags,
           'ps5/tests/native_engine_shim_test.cpp', 'ps5/tests/native_engine_shim_fixture.S',
           'ps5/native_engine_memory.cpp', 'ps5/native_engine_entry.S',
           str(output / 'thunks.o'), str(output / 'files.o'), str(output / 'stubs.S'),
           '-Wl,--gc-sections', '-pthread', '-o', str(output / 'engine-shim')]
subprocess.run(command, cwd=root, check=True)
subprocess.run([str(output / 'engine-shim')], cwd=root, check=True)
spec = importlib.util.spec_from_file_location('asm64', root / 'ps5/tools/asm64_prepare.py')
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)
pattern = re.compile(r'^\s*(arg_[\w]+|argc|argv|envp)\s*=\s*'
                     r'(?:byte|word|dword|qword|tbyte|xmmword)\s+ptr\s+([0-9a-f]+h|[0-9]+)\s*$', re.I)
count = 0
for line in (root / 'ASM_Sources/bedlam2.asm').read_text(errors='replace').splitlines():
    match = pattern.match(line.partition(';')[0])
    if match:
        token = match[2]
        before = int(token[:-1], 16) if token.lower().endswith('h') else int(token)
        translated = converter.transform_line(line, set(), set(), {}, {})
        after = int(pattern.match(translated.partition(';')[0])[2])
        assert after == before + 4, line
        count += 1
local = 'var_4 = dword ptr -4'
assert converter.transform_line(local, set(), set(), {}, {}) == local
assert count == 179
print(f'Formal-argument checks passed: {count} named stack slots shifted by four; locals unchanged.')
