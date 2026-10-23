#!/usr/bin/env python3
"""Build an experimental x86-64 ELF object from the legacy MASM source.

This is a syntax and code-generation probe, not a runnable PS5 engine. The
game still needs low-address engine memory, DOS/runtime replacements, and
native ABI thunks before this object can be linked into a title.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


REG32_TO_64 = {
    "eax": "rax",
    "ebx": "rbx",
    "ecx": "rcx",
    "edx": "rdx",
    "esi": "rsi",
    "edi": "rdi",
    "ebp": "rbp",
    "esp": "rsp",
}

REG16_TO_32 = {
    "ax": "eax",
    "bx": "ebx",
    "cx": "ecx",
    "dx": "edx",
    "si": "esi",
    "di": "edi",
    "bp": "ebp",
    "sp": "esp",
}

# The recovered engine's external calls use 32-bit cdecl stack slots. Route
# them through these SysV adapters so the original arguments and callee-save
# expectations survive on the PS5 x86-64 ABI.
LEGACY_EXTERNAL_THUNKS = {
    "clear_screen": ("clear_screen", 0),
    "copy_buffer_to_screen_and_unlock": ("copy_buffer_to_screen_and_unlock", 1),
    "copy_screen_to_buffer": ("copy_screen_to_buffer", 1),
    "correct_screen_pos": ("correct_screen_pos", 0),
    "draw_map_and_game_screen": ("draw_map_and_game_screen", 0),
    "exit": ("exit", 1),
    "fclose": ("fclose", 1),
    "fflush": ("fflush", 1),
    "fopen": ("fopen", 2),
    "fread": ("fread", 4),
    "free": ("bedlam2_ps5_free", 1),
    "free_win": ("free_win", 0),
    "shop_loop_started": ("bedlam2_ps5_shop_loop_started", 0),
    "fseek": ("fseek", 3),
    "ftell": ("ftell", 1),
    "fwrite": ("fwrite", 4),
    "fill_player_name": ("bedlam2_ps5_fill_player_name", 2),
    "get_screen_buffer_ptr": ("get_screen_buffer_ptr", 0),
    "getcwd": ("getcwd", 2),
    "hide_cursor": ("hide_cursor", 0),
    "load_music": ("load_music", 1),
    "load_raw": ("load_raw", 1),
    "malloc": ("bedlam2_ps5_malloc", 1),
    "mkdir": ("mkdir", 1),
    "play_music": ("play_music", 4),
    "play_sound": ("play_sound", 4),
    "play_wav_music": ("play_wav_music", 1),
    "printf": ("printf", 6),
    "putchar": ("putchar", 1),
    "puts": ("puts", 1),
    "redraw": ("redraw", 0),
    "redraw_copy_buff_to_screen_and_unlock": (
        "redraw_copy_buff_to_screen_and_unlock", 1
    ),
    "set_palette": ("set_palette", 1),
    "set_volume": ("set_volume", 1),
    "show_cursor": ("show_cursor", 0),
    "stop_music": ("stop_music", 0),
    "stop_wav_music": ("stop_wav_music", 0),
    "swap_palette_with_animation": ("swap_palette_with_animation", 2),
    "unlock_surface_and_wait": ("unlock_surface_and_wait", 1),
    "wait_palette_animation": ("wait_palette_animation", 1),
    "sdl_events": ("SDL_events", 0),
}

PS5_RUNTIME_PROC_REPLACEMENTS = {
    # The weapon shop calls short_wait(5000) before every redraw. That legacy
    # routine polls SDL 5000 times; the native redraw path already pumps SDL
    # events and samples the controller once per frame, so omit the busy loop.
    "short_wait": (),
    "lseek_": ("                call bedlam2_ps5_dos_lseek_bridge",),
    "__qwrite_": ("                call bedlam2_ps5_dos_qwrite_bridge",),
    "isatty_": ("                call bedlam2_ps5_dos_isatty_bridge",),
    # These Watcom startup hooks only allocate/free DPMI selectors in DOS.
    "init_": (),
    "fini_": (),
}


def extract_embedded_data(source: str) -> tuple[str, list[str], set[str]]:
    """Move actual data islands out of PS5 execute-only text.

    Bare DB directives elsewhere encode instruction prefixes/opcodes and must
    stay executable. These two named blocks are executable too: startup NOPs
    and a DOS interrupt trampoline. Leave them for the separate DOS-code audit.
    """
    executable_blocks = {"___begtext", "word_6d1c2"}
    code_lines: list[str] = []
    data_lines: list[str] = []
    data_symbols: set[str] = set()
    in_data = False
    for line in source.splitlines():
        code = line.partition(";")[0].strip()
        declaration = re.match(
            r"^([A-Za-z_.$?@][\w.$?@]*)\s+(db|dw|dd|dq)\b", code, re.I
        )
        if declaration:
            symbol = declaration.group(1).lower()
            in_data = symbol not in executable_blocks
            if in_data:
                data_symbols.add(symbol)
        elif code and not re.match(r"^(db|dw|dd|dq|align)\b", code, re.I):
            in_data = False
        if in_data and code:
            data_lines.append(line)
            # Preserve code alignment too, including alignment after a table.
            if re.match(r"^align\b", code, re.I):
                code_lines.append(line)
        else:
            code_lines.append(line)
    return "\n".join(code_lines), data_lines, data_symbols


def source_symbols(
    source_dir: Path,
) -> tuple[set[str], set[str], dict[str, str], dict[str, str]]:
    code = (source_dir / "bedlam2.asm").read_text(errors="replace")
    data = (source_dir / "bedlam2_data.inc").read_text(errors="replace")
    imports = (source_dir / "bedlam2.inc").read_text(errors="replace")
    proc_names = {
        match.group(1).lower()
        for match in re.finditer(
            r"^\s*([A-Za-z_.$?@][\w.$?@]*)\s+proc\b", code, re.I | re.M
        )
    }
    data_names = {
        match.group(1).lower()
        for match in re.finditer(
            r"^\s*([A-Za-z_.$?@][\w.$?@]*)\s+(?:dword|dd|word|dw|byte|db)\b",
            data,
            re.I | re.M,
        )
    }
    data_types: dict[str, str] = {}
    data_sections: dict[str, str] = {}
    section_by_file = {
        "bedlam2.asm": "text",
        "bedlam2_data.inc": "data",
        "bedlam2.inc": "external",
    }
    for filename, initial_section in section_by_file.items():
        section = initial_section
        for line in (source_dir / filename).read_text(errors="replace").splitlines():
            directive = re.match(r"^\s*\.(data\??|code)(?=\s|$)", line, re.I)
            if directive:
                section = {
                    ".data": "data",
                    ".data?": "bss",
                    ".code": "text",
                }["." + directive.group(1).lower()]
            declaration = re.match(
                r"^\s*([A-Za-z_.$?@][\w.$?@]*)\s+(dword|dd|word|dw|byte|db)\b",
                line,
                re.I,
            )
            if declaration:
                symbol = declaration.group(1).lower()
                name = declaration.group(2).lower()
                data_types[symbol] = {
                    "dword": "dword",
                    "dd": "dword",
                    "word": "word",
                    "dw": "word",
                    "byte": "byte",
                    "db": "byte",
                }[name]
                data_sections[symbol] = section
    return proc_names, data_names, data_types, data_sections


def source_instruction_names(path: Path) -> set[str]:
    """Collect real instruction mnemonics for handling code islands between PROC blocks."""
    names: set[str] = set()
    in_proc = False
    directives = {"db", "dw", "dd", "dq", "byte", "word", "dword", "align", "assume"}
    for line in path.read_text(errors="replace").splitlines():
        if re.match(r"^\s*[A-Za-z_.$?@][\w.$?@]*\s+proc\b", line, re.I):
            in_proc = True
            continue
        if re.match(r"^\s*[A-Za-z_.$?@][\w.$?@]*\s+endp\b", line, re.I):
            in_proc = False
            continue
        if not in_proc:
            continue
        code = line.partition(";")[0].strip()
        if not code:
            continue
        fields = code.split(None, 2)
        if fields[0].endswith(":") or len(fields) > 1 and fields[1].lower() in directives:
            continue
        names.add(fields[0].lower())
    return names


def push32(indent: str, operand: str) -> str:
    low = operand.lower().strip()
    if low in REG32_TO_64 and low != "esp":
        return f"{indent}lea rsp, [rsp-4]\n{indent}mov dword ptr [rsp], {low}"
    if low == "esp":
        return (
            f"{indent}lea rsp, [rsp-4]\n"
            f"{indent}lea r11, [rsp+4]\n"
            f"{indent}mov dword ptr [rsp], r11d"
        )
    if low in {"ds", "es", "ss", "cs"}:
        return f"{indent}lea rsp, [rsp-4]\n{indent}mov dword ptr [rsp], 0"
    if low in {"fd", "flags", "eflags"}:
        return (
            f"{indent}pushfq\n{indent}pop r11\n"
            f"{indent}lea rsp, [rsp-4]\n{indent}mov dword ptr [rsp], r11d"
        )
    if low.startswith("offset "):
        value = operand.strip()[7:].strip()
        source = f"offset {value}"
    elif re.match(r"^(?:byte|word|dword|qword)\s+ptr\b", low):
        source = operand
    elif re.fullmatch(r"[-+]?\s*(?:0[0-9a-f]+h|[0-9]+|[0-9a-f]+h)", low):
        return f"{indent}lea rsp, [rsp-4]\n{indent}mov dword ptr [rsp], {operand}"
    else:
        source = operand
    return (
        f"{indent}mov r11d, {source}\n"
        f"{indent}lea rsp, [rsp-4]\n"
        f"{indent}mov dword ptr [rsp], r11d"
    )


def pop32(indent: str, operand: str) -> str:
    low = operand.lower().strip()
    if low in REG32_TO_64 and low != "esp":
        return f"{indent}mov {low}, dword ptr [rsp]\n{indent}lea rsp, [rsp+4]"
    if low == "esp":
        return (
            f"{indent}mov r11d, dword ptr [rsp]\n"
            f"{indent}lea rsp, [rsp+4]\n"
            f"{indent}mov esp, r11d"
        )
    if low in {"ds", "es", "ss", "cs"}:
        return f"{indent}lea rsp, [rsp+4]"
    if low in {"fd", "flags", "eflags"}:
        return (
            f"{indent}mov r11d, dword ptr [rsp]\n"
            f"{indent}lea rsp, [rsp+4]\n"
            f"{indent}push r11\n{indent}popfq"
        )
    if low in REG16_TO_32:
        return f"{indent}mov {low}, word ptr [rsp]\n{indent}lea rsp, [rsp+2]"
    return (
        f"{indent}mov r11d, dword ptr [rsp]\n"
        f"{indent}lea rsp, [rsp+4]\n"
        f"{indent}mov {operand}, r11d"
    )


def pusha(indent: str) -> str:
    return "\n".join(
        (
            f"{indent}lea rsp, [rsp-32]",
            f"{indent}mov dword ptr [rsp+28], eax",
            f"{indent}mov dword ptr [rsp+24], ecx",
            f"{indent}mov dword ptr [rsp+20], edx",
            f"{indent}mov dword ptr [rsp+16], ebx",
            f"{indent}lea r11, [rsp+32]",
            f"{indent}mov dword ptr [rsp+12], r11d",
            f"{indent}mov dword ptr [rsp+8], ebp",
            f"{indent}mov dword ptr [rsp+4], esi",
            f"{indent}mov dword ptr [rsp], edi",
        )
    )


def popa(indent: str) -> str:
    return "\n".join(
        (
            f"{indent}mov edi, dword ptr [rsp]",
            f"{indent}mov esi, dword ptr [rsp+4]",
            f"{indent}mov ebp, dword ptr [rsp+8]",
            f"{indent}mov ebx, dword ptr [rsp+16]",
            f"{indent}mov edx, dword ptr [rsp+20]",
            f"{indent}mov ecx, dword ptr [rsp+24]",
            f"{indent}mov eax, dword ptr [rsp+28]",
            f"{indent}lea rsp, [rsp+32]",
        )
    )


def make_indexed_memory_pic(
    code: str, data_types: dict[str, str], data_sections: dict[str, str]
) -> str:
    """Use a PIE-relative section base for indexed static data accesses."""
    register_map = {
        "eax": "rax",
        "ebx": "rbx",
        "ecx": "rcx",
        "edx": "rdx",
        "esi": "rsi",
        "edi": "rdi",
        "ebp": "rbp",
        "esp": "rsp",
    }
    pattern = re.compile(
        r"(?<![\w.$?@])(?:\(\s*(?:offset\s+)?([A-Za-z_.$?@][\w.$?@]*)"
        r"\s*([+-]\s*(?:[0-9a-f]+h|[0-9]+))?\s*\)"
        r"|([A-Za-z_.$?@][\w.$?@]*))\[([^\[\]]+)\]",
        re.I,
    )
    match = pattern.search(code)
    if not match:
        return code
    symbol = match.group(1) or match.group(3)
    addend = match.group(2) or ""
    index = match.group(4)
    if symbol.lower() == "pressed_key_arr":
        index64 = re.sub(
            r"\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b",
            lambda reg: register_map[reg.group(1).lower()],
            index,
            flags=re.I,
        )
        lea = re.match(
            r"^(\s*lea\s+)(e(?:ax|bx|cx|dx|si|di|bp|sp))\s*,", code, re.I
        )
        load_base = (
            "                lea r11, PRESSED_KEY_ARR\n"
            "                mov r11, qword ptr [r11]"
        )
        if lea:
            destination = register_map[lea.group(2).lower()]
            replacement = f"[r11+{index64}{addend}]"
            rewritten = code[: match.start()] + replacement + code[match.end() :]
            rewritten = re.sub(
                r"^\s*lea\s+e(?:ax|bx|cx|dx|si|di|bp|sp)",
                f"                lea {destination}",
                rewritten,
                count=1,
                flags=re.I,
            )
        else:
            wrapped = (
                match.start() > 0
                and code[match.start() - 1] == "["
                and match.end() < len(code)
                and code[match.end()] == "]"
            )
            replacement = (
                f"r11+{index64}{addend}"
                if wrapped
                else f"byte ptr [r11+{index64}{addend}]"
            )
            rewritten = code[: match.start()] + replacement + code[match.end() :]
        return load_base + "\n" + rewritten
    if symbol.lower() not in data_types:
        return code

    # A LEA produces an address rather than reading the global array. Keep its
    # result in a 64-bit register so it remains valid after PIE relocation.
    lea = re.match(r"^(\s*lea\s+)(e(?:ax|bx|cx|dx|si|di|bp|sp))\s*,", code, re.I)
    index64 = re.sub(
        r"\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b",
        lambda reg: register_map[reg.group(1).lower()],
        index,
        flags=re.I,
    )
    section = data_sections.get(symbol.lower(), "external")
    section_bases = {"data": "r14", "bss": "r13", "text": "r15"}
    base = section_bases.get(section)
    high_bytes = {
        reg for reg in ("ah", "bh", "ch", "dh")
        if re.search(rf"\b{reg}\b", code, re.I)
    }
    if high_bytes:
        parent_regs = {"ah": "eax", "bh": "ebx", "ch": "ecx", "dh": "edx"}
        used = {
            reg.lower()
            for reg in re.findall(
                r"\b(?:eax|ebx|ecx|edx|esi|edi|ebp|esp)\b", code, re.I
            )
        }
        used.update(parent_regs[reg] for reg in high_bytes)
        available = [
            reg for reg in ("ebx", "ecx", "edx", "esi", "edi", "ebp", "eax")
            if reg not in used
        ]
        index_registers = re.findall(
            r"\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b", index, re.I
        )
        need_offset = len({reg.lower() for reg in index_registers}) > 1
        if len(available) >= (2 if need_offset else 1):
            base_scratch = available[0]
            offset_scratch = available[1] if need_offset else ""
            base64 = register_map[base_scratch]
            before = [
                "                lea rsp, [rsp-4]",
                f"                mov dword ptr [rsp], {base_scratch}",
            ]
            if offset_scratch:
                before += [
                    "                lea rsp, [rsp-4]",
                    f"                mov dword ptr [rsp], {offset_scratch}",
                ]
            if base:
                before.append(
                    f"                lea {base64}, [{base}+{symbol}-bedlam2_{section}_base{addend}]"
                )
            else:
                before.append(f"                lea {base64}, {symbol}")
            if offset_scratch:
                offset64 = register_map[offset_scratch]
                before.append(f"                lea {offset64}, [{index64}]")
                effective_index = offset64
            else:
                effective_index = index64

            wrapped = (
                match.start() > 0
                and code[match.start() - 1] == "["
                and match.end() < len(code)
                and code[match.end()] == "]"
            )
            if wrapped:
                replacement = f"{base64}+{effective_index}{addend}"
            else:
                replacement = f"{data_types[symbol.lower()]} ptr [{base64}+{effective_index}{addend}]"
            rewritten = code[: match.start()] + replacement + code[match.end() :]
            after = []
            if offset_scratch:
                after += [
                    f"                mov {offset_scratch}, dword ptr [rsp]",
                    "                lea rsp, [rsp+4]",
                ]
            after += [
                f"                mov {base_scratch}, dword ptr [rsp]",
                "                lea rsp, [rsp+4]",
            ]
            return "\n".join(before + [rewritten] + after)

    if base:
        displacement = f"{symbol}-bedlam2_{section}_base{addend}"
        index_registers = re.findall(
            r"\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b", index, re.I
        )
        offset_prelude = ""
        if len(set(r.lower() for r in index_registers)) > 1:
            offset_prelude = f"                lea r10, [{index64}]\n"
            index64 = "r10"
        if lea:
            destination = register_map[lea.group(2).lower()]
            replacement = f"[{base}+{displacement}+{index64}]"
            rewritten = code[: match.start()] + replacement + code[match.end() :]
            rewritten = re.sub(
                r"^\s*lea\s+e(?:ax|bx|cx|dx|si|di|bp|sp)",
                f"{lea.group(1)}{destination}",
                rewritten,
                count=1,
                flags=re.I,
            )
            return offset_prelude + rewritten

        # If SYMBOL[index] is already wrapped in a memory operand, retain its
        # existing width qualifier and outer brackets.
        wrapped = match.start() > 0 and code[match.start() - 1] == "[" and match.end() < len(code) and code[match.end()] == "]"
        if wrapped:
            replacement = f"{base}+{displacement}+{index64}"
            start, end = match.start(), match.end()
        else:
            replacement = f"{data_types[symbol.lower()]} ptr [{base}+{displacement}+{index64}]"
            start, end = match.start(), match.end()
        rewritten = code[:start] + replacement + code[end:]
        return offset_prelude + rewritten

    if lea:
        destination = register_map[lea.group(2).lower()]
        replacement = f"[r11+{index64}{addend}]"
        rewritten = code[: match.start()] + replacement + code[match.end() :]
        rewritten = re.sub(r"^\s*lea\s+e(?:ax|bx|cx|dx|si|di|bp|sp)",
                            f"{lea.group(1)}{destination}", rewritten, count=1, flags=re.I)
        return f"                lea r11, {symbol}\n{rewritten}"

    replacement = f"{data_types[symbol.lower()]} ptr [r11+{index64}{addend}]"
    rewritten = code[: match.start()] + replacement + code[match.end() :]
    return f"                lea r11, {symbol}\n{rewritten}"


def make_direct_memory_pic(
    code: str,
    data_types: dict[str, str],
    data_sections: dict[str, str],
) -> str:
    """Address local engine globals through the reserved low-data base registers."""
    if not re.match(r"^\s*[a-z][a-z0-9]*\b", code, re.I):
        return code

    if re.search(r"\blea\s+r11\s*,\s*PRESSED_KEY_ARR\b", code, re.I):
        return code
    pressed_ref = re.search(
        r"(?<![\w.$?@])PRESSED_KEY_ARR"
        r"(?:\s*([+-])\s*(0[0-9a-f]+h|[0-9a-f]+h|[0-9]+))?",
        code,
        re.I,
    )
    if pressed_ref:
        sign, amount = pressed_ref.groups()
        displacement = "0"
        if sign and amount:
            value = int(amount[:-1], 16) if amount.lower().endswith("h") else int(amount)
            displacement = f"{value if sign == '+' else -value}"
        before = code[: pressed_ref.start()]
        after = code[pressed_ref.end() :]
        left_bracket = code.rfind("[", 0, pressed_ref.start())
        left_close = code.rfind("]", 0, pressed_ref.start())
        right_bracket = code.find("]", pressed_ref.end())
        inside_brackets = left_bracket > left_close and right_bracket >= pressed_ref.end()
        replacement = (
            f"r11+{displacement}"
            if inside_brackets
            else f"byte ptr [r11+{displacement}]"
        )
        rewritten = before + replacement + after
        return (
            "                lea r11, PRESSED_KEY_ARR\n"
            "                mov r11, qword ptr [r11]\n"
            + rewritten
        )

    matches = list(re.finditer(
        r"(?<![\w.$?@])([A-Za-z_.$?@][\w.$?@]*)(?![\w.$?@])", code
    ))
    for match in reversed(matches):
        symbol = match.group(1)
        symbol_key = symbol.lower()
        section = data_sections.get(symbol_key)
        if section not in {"data", "bss"}:
            continue

        # OFFSET values are address expressions handled separately. Likewise,
        # do not rewrite expressions that the indexed-symbol pass already made
        # relative to one of these bases.
        before = code[: match.start()]
        after = code[match.end() :]
        if re.search(r"\boffset\s*$", before, re.I):
            continue
        if re.match(r"\s*-\s*bedlam2_(?:data|bss)_base\b", after, re.I):
            continue

        base = "r14" if section == "data" else "r13"
        displacement = f"{symbol}-bedlam2_{section}_base"
        expression = f"{base}+{displacement}"

        left_bracket = code.rfind("[", 0, match.start())
        left_close = code.rfind("]", 0, match.start())
        right_bracket = code.find("]", match.end())
        inside_brackets = left_bracket > left_close and right_bracket >= match.end()
        if inside_brackets:
            replacement = expression
        else:
            qualifier = re.search(r"\b(byte|word|dword|qword)\s+ptr\s*$", before, re.I)
            if qualifier:
                replacement = f"[{expression}]"
            elif re.match(r"^\s*lea\b", code, re.I):
                replacement = f"[{expression}]"
            else:
                replacement = f"{data_types[symbol_key]} ptr [{expression}]"
        code = code[: match.start()] + replacement + code[match.end() :]

    return code


def make_offset_address_pic(
    code: str,
    data_sections: dict[str, str],
    proc_names: set[str],
) -> str:
    """Form low 32-bit addresses for OFFSET operands naming engine globals."""
    output: list[str] = []
    offset_pattern = re.compile(
        r"\(?\s*offset\s+([A-Za-z_.$?@][\w.$?@]*)"
        r"(?:\s*([+-])\s*(0[0-9a-f]+h|[0-9a-f]+h|[0-9]+))?\s*\)?",
        re.I,
    )
    for line in code.split("\n"):
        match = offset_pattern.search(line)
        if not match:
            output.append(line)
            continue

        symbol, sign, amount = match.groups()
        section = data_sections.get(symbol.lower())
        simple_move = re.match(
            r"^(\s*)mov\s+(e(?:ax|bx|cx|dx|si|di|bp))\s*,\s*"
            r"\(?\s*offset\s+[A-Za-z_.$?@][\w.$?@]*"
            r"(?:\s*[+-]\s*(?:0[0-9a-f]+h|[0-9a-f]+h|[0-9]+))?\s*\)?\s*$",
            line,
            re.I,
        )
        if symbol.lower() == "byte_14723" and section == "text" and simple_move:
            indent, destination = simple_move.groups()
            output.append(
                f"{indent}mov {destination.lower()}, "
                "bedlam2_ps5_symbol_search_base_low"
            )
            continue
        if symbol.lower() in proc_names and simple_move:
            indent, destination = simple_move.groups()
            address = f"{symbol} - bedlam2_text_base"
            if sign and amount:
                addend = int(amount[:-1], 16) if amount.lower().endswith("h") else int(amount)
                address += f" {'+' if sign == '+' else '-'} {addend}"
            output.append(f"{indent}mov {destination.lower()}, {address}")
            continue
        table_base_add = re.match(
            r"^(\s*)add\s+(e(?:ax|bx|cx|dx|si|di|bp))\s*,\s*"
            r"\(?\s*offset\s+([A-Za-z_.$?@][\w.$?@]*)\s*\)?\s*$",
            line,
            re.I,
        )
        if section == "text" and table_base_add:
            indent, destination, table = table_base_add.groups()
            full_destination = REG32_TO_64[destination.lower()]
            output.append(
                f"{indent}lea {full_destination}, "
                f"[r15+{table}-bedlam2_text_base+{full_destination}]"
            )
            continue
        if symbol.lower() in proc_names or re.match(r"^(?:loc|def)_", symbol, re.I):
            replacement = f"{symbol} - bedlam2_text_base"
            if sign and amount:
                addend = int(amount[:-1], 16) if amount.lower().endswith("h") else int(amount)
                replacement += f" {'+' if sign == '+' else '-'} {addend}"
            output.append(line[: match.start()] + replacement + line[match.end() :])
            continue
        if section not in {"data", "bss"}:
            output.append(line)
            continue

        base = "r14" if section == "data" else "r13"
        address = f"{base}+{symbol}-bedlam2_{section}_base"
        if sign and amount:
            addend = int(amount[:-1], 16) if amount.lower().endswith("h") else int(amount)
            address += f"{'+' if sign == '+' else '-'}{addend}"
        scratch_move = re.match(
            r"^(\s*)mov\s+r11d\s*,\s*\(?\s*offset\s+"
            r"[A-Za-z_.$?@][\w.$?@]*"
            r"(?:\s*[+-]\s*(?:0[0-9a-f]+h|[0-9a-f]+h|[0-9]+))?\s*\)?\s*$",
            line,
            re.I,
        )
        if simple_move:
            indent, destination = simple_move.groups()
            output.append(f"{indent}lea {destination.lower()}, [{address}]")
            continue
        if scratch_move:
            indent = scratch_move.group(1)
            output.append(f"{indent}lea r11d, [{address}]")
            continue

        rewritten = line[: match.start()] + "r11d" + line[match.end() :]
        output.append(f"                lea r11d, [{address}]")
        output.append(rewritten)
    return "\n".join(output)


def bridge_high_byte_with_extended_memory(code: str) -> str:
    """Avoid AH/BH/CH/DH in instructions that need a REX address prefix."""
    store = re.match(
        r"^(\s*)mov\s+(byte\s+ptr\s+\[[^\]]+\])\s*,\s*(ah|bh|ch|dh)\s*$",
        code,
        re.I,
    )
    if store:
        indent, memory, source = store.groups()
        return "\n".join((
            f"{indent}push rbx",
            f"{indent}mov bl, {source.lower()}",
            f"{indent}mov {memory}, bl",
            f"{indent}pop rbx",
        ))

    load = re.match(
        r"^(\s*)mov\s+(ah|bh|ch|dh)\s*,\s*(byte\s+ptr\s+\[[^\]]+\])\s*$",
        code,
        re.I,
    )
    if load:
        indent, destination, memory = load.groups()
        return "\n".join((
            f"{indent}push rbx",
            f"{indent}mov bl, {memory}",
            f"{indent}mov {destination.lower()}, bl",
            f"{indent}pop rbx",
        ))
    return code


def transform_line(
    line: str,
    proc_names: set[str],
    data_names: set[str],
    data_types: dict[str, str],
    data_sections: dict[str, str],
    current_proc: str = "",
    in_jump_table: bool = False,
    rewrite_data_operands: bool = False,
) -> str:
    code, separator, comment = line.partition(";")

    # Named parameters sit above the return address. Hardware CALL/RET use
    # eight bytes in long mode, while legacy saved registers/arguments stay
    # four bytes. Locals below the saved frame must keep their original offsets.
    parameter = re.match(
        r"^(\s*(?:arg_[\w]+|argc|argv|envp)\s*=\s*"
        r"(?:byte|word|dword|qword|tbyte|xmmword)\s+ptr\s+)"
        r"([0-9a-f]+h|[0-9]+)(\s*)$", code, re.I,
    )
    if parameter:
        token = parameter.group(2)
        offset = int(token[:-1], 16) if token.lower().endswith("h") else int(token)
        if offset > 0:
            code = parameter.group(1) + str(offset + 4) + parameter.group(3)

    if (
        current_proc.lower() == "sub_66a46"
        and re.match(r"^\s*int\s+21h\b", code, re.I)
    ):
        code = re.sub(
            r"^\s*int\s+21h\b",
            "                call bedlam2_ps5_keyboard_status_bridge",
            code,
            count=1,
            flags=re.I,
        )

    if re.fullmatch(r"\s*\.586\s*", code, re.I):
        code = ".x64"
    elif re.fullmatch(r"\s*\.model\s+flat\s*,\s*C\s*", code, re.I):
        code = ""

    # Segment overrides and explicit segment changes belong to the DOS flat model.
    code = re.sub(r"\b(?:ds|cs|es|ss):", "", code, flags=re.I)
    code = re.sub(r"\bshort\s+", "", code, flags=re.I)
    code = re.sub(
        r"^(\s*j(?:ecxz|cxz|rcxz|loop[a-z]*)\s+)(?:near ptr\s+)?",
        r"\1short ",
        code,
        flags=re.I,
    )

    # Preserve host-stack pointers in 64-bit registers. Legacy 32-bit stack
    # arithmetic still uses the same byte deltas, while address expressions
    # must use 64-bit base/index registers in long mode.
    stack_op = re.match(
        r"^(\s*(?:add|sub|inc|dec|and|or|xor|neg|not)\s+)esp\b",
        code,
        re.I,
    )
    if stack_op:
        code = code[: stack_op.start()] + stack_op.group(1) + "rsp" + code[stack_op.end() :]
    code = re.sub(r"^(\s*lea\s+)esp\b", r"\1rsp", code, flags=re.I)
    stack_mov = re.match(
        r"^(\s*mov\s+)(e(?:ax|bx|cx|dx|si|di|bp|sp))\s*,\s*"
        r"(e(?:ax|bx|cx|dx|si|di|bp|sp))\s*$",
        code,
        re.I,
    )
    if stack_mov and "esp" in {
        stack_mov.group(2).lower(), stack_mov.group(3).lower()
    }:
        code = (
            stack_mov.group(1)
            + REG32_TO_64[stack_mov.group(2).lower()]
            + ", "
            + REG32_TO_64[stack_mov.group(3).lower()]
        )

    # In long mode these instructions only matter to the DOS runtime, which uses
    # flat selectors here. Preserve an explicit move's destination value harmlessly.
    match = re.match(r"^(\s*)mov\s+(ds|es|ss|cs),\s*(.+)$", code, re.I)
    if match:
        code = f"{match.group(1)}nop"
    match = re.match(
        r"^(\s*)mov\s+(e?[abcd]x|e?[sd]i|e?[sb]p),\s*(ds|es|ss|cs)\s*$",
        code,
        re.I,
    )
    if match:
        code = f"{match.group(1)}mov {match.group(2)}, 0"

    # LES/LDS are DOS selector loads; keep the flat 32-bit pointer portion only.
    match = re.match(r"^(\s*)(les|lds)\s+(e[a-z]{2}),\s*(.+)$", code, re.I)
    if match:
        code = f"{match.group(1)}mov {match.group(3).lower()}, dword ptr {match.group(4)}"

    match = re.match(r"^(\s*)(push|pop)\s+(.+?)\s*$", code, re.I)
    if match:
        indent, operation, operand = match.groups()
        return_address = re.match(
            r"^offset\s+((?:loc|def)_[A-Za-z0-9_.$?@]+)"
            r"(?:\s*([+-])\s*(0[0-9a-f]+h|[0-9a-f]+h|[0-9]+))?$",
            operand.strip(),
            re.I,
        )
        if operation.lower() == "push" and return_address:
            symbol, sign, amount = return_address.groups()
            displacement = f"{symbol}-bedlam2_text_base"
            if sign and amount:
                addend = int(amount[:-1], 16) if amount.lower().endswith("h") else int(amount)
                displacement += f" {'+' if sign == '+' else '-'} {addend}"
            code = f"{indent}lea r11, [r15+{displacement}]\n{indent}push r11"
        else:
            code = push32(indent, operand) if operation.lower() == "push" else pop32(indent, operand)

    if re.fullmatch(r"\s*pusha\s*", code, re.I):
        code = pusha("                ")
    elif re.fullmatch(r"\s*popa\s*", code, re.I):
        code = popa("                ")

    # LEA into a 32-bit destination truncates a host stack address. Keep the
    # full pointer when the expression is based on ESP/EBP.
    lea_stack = re.match(
        r"^(\s*lea\s+)(e(?:ax|bx|cx|dx|si|di|bp))\s*,\s*\[([^\]]*)\]",
        code,
        re.I,
    )
    if lea_stack and re.search(r"\b(?:esp|ebp)\b", lea_stack.group(3), re.I):
        code = re.sub(
            r"^(\s*lea\s+)e(?:ax|bx|cx|dx|si|di|bp)\b",
            lambda match: match.group(1)
            + REG32_TO_64[
                re.search(
                    r"e(?:ax|bx|cx|dx|si|di|bp)", match.group(0), re.I
                ).group(0).lower()
            ],
            code,
            count=1,
            flags=re.I,
        )

    # The original jump/call tables contain 32-bit function addresses.
    match = re.match(r"^(\s*)(call|jmp)\s+(?:dword ptr\s+)?(.+?)\s*$", code, re.I)
    if match:
        indent, operation, target = match.groups()
        target = target.strip()
        low_allocator = {
            ("_nmalloc_", "malloc"): "bedlam2_ps5_malloc",
            ("_nfree_", "free"): "bedlam2_ps5_free",
        }.get((current_proc, target.lower()))
        if operation.lower() == "call" and low_allocator:
            code = f"{indent}mov edi, eax\n{indent}call {low_allocator}"
            target = ""
        if target:
            match = re.match(r"^([A-Za-z_.$?@][\w.$?@]*)", target)
            symbol = match.group(1).lower() if match else ""
        else:
            symbol = ""
        if operation.lower() == "call" and symbol in LEGACY_EXTERNAL_THUNKS:
            code = f"{indent}call bedlam2_ps5_legacy_{symbol}"
            target = ""
        if target:
            text_jump = (
                operation.lower() == "jmp"
                and target.lower() == "eax"
                and current_proc in {"sub_73aa0", "sub_74c70"}
            )
            jump_table = operation.lower() == "jmp" and re.match(
                r"^jpt_[\w.$?@]*\s*\[", target, re.I
            )
            text_callback = operation.lower() == "call" and (
                symbol in {"dword_127c60", "dword_127c48", "dword_82afc"}
                or current_proc == "qsort_" and "var_10" in target.lower()
                or current_proc == "med3_" and "var_4" in target.lower()
                or current_proc == "__prtf_" and "var_18" in target.lower()
                or current_proc == "float_format_" and symbol == "off_83b40"
                or current_proc == "sub_6c8ad"
                and re.search(r"\[(?:esi|edi|eax|ecx)\+(?:5ch|60h)\]", target, re.I)
            )
            if target.lower() in REG32_TO_64:
                code = (
                    f"{indent}add rax, r15\n" if text_jump else ""
                ) + f"{indent}{operation.lower()} {REG32_TO_64[target.lower()]}"
            elif "[" in target or (symbol in data_names and symbol not in proc_names):
                operand = target if target.startswith("[") else f"[{target}]"
                operand = re.sub(r"\besp\b", "rsp", operand, flags=re.I)
                trailer = (
                    f"{indent}add r11, r15\n"
                    if jump_table or text_callback else ""
                )
                code = (
                    f"{indent}mov r11d, dword ptr {operand}\n"
                    f"{trailer}{indent}{operation.lower()} r11"
                )

    # The recovered switch tables are 32-bit code offsets. Keep each table
    # entry relative to the text-section base and add the runtime PIE base at
    # the indirect jump site. This removes absolute relocations from the tables.
    if in_jump_table:
        code = re.sub(
            r"\boffset\s+((?:loc|def)_[A-Za-z0-9_.$?@]+)",
            r"\1 - bedlam2_text_base",
            code,
            flags=re.I,
        )

    # robot_damage accepts -1 as a sentinel robot slot. The original 32-bit
    # effective address sign-extends that computed offset; x64 indexed memory
    # operands use the zero-extended RAX value unless we restore the sign here.
    if current_proc.lower() == "robot_damage" and re.match(
        r"^\s*mov\s+ebx\s*,\s*dword_B6184\s*\[\s*eax\s*\]\s*$",
        code,
        re.I,
    ):
        code = "                movsxd rax, eax\n" + code

    # The 32-bit encoding for SYMBOL[index] is an absolute disp32 relocation in
    # long mode. Materialize SYMBOL with RIP-relative LEA and use a 64-bit base.
    code = "\n".join(
        make_indexed_memory_pic(part, data_types, data_sections)
        for part in code.split("\n")
    )
    if rewrite_data_operands:
        code = make_offset_address_pic(code, data_sections, proc_names)
        code = make_direct_memory_pic(code, data_types, data_sections)
    code = re.sub(
        r"\[([^\]]*)\]",
        lambda match: "[" + re.sub(
            r"\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b",
            lambda reg: REG32_TO_64[reg.group(1).lower()],
            match.group(1),
            flags=re.I,
        ) + "]",
        code,
    )
    code = "\n".join(
        bridge_high_byte_with_extended_memory(part)
        for part in code.split("\n")
    )

    if separator:
        lines = code.split("\n")
        lines[0] += " " + separator + comment
        return "\n".join(lines)
    return code


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=root / "ASM_Sources")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--jwasm", type=Path, required=True)
    args = parser.parse_args()

    source_dir = args.source_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    proc_names, data_names, data_types, data_sections = source_symbols(source_dir)
    instruction_names = source_instruction_names(source_dir / "bedlam2.asm")
    code_source, embedded_data, embedded_symbols = extract_embedded_data(
        (source_dir / "bedlam2.asm").read_text(errors="replace")
    )
    # Tables now live outside their original PROC scope. Their destination
    # labels must be visible globally, including forward references from DATA.
    embedded_targets = {
        match.group(1).lower()
        for line in embedded_data
        for match in re.finditer(r"\boffset\s+([A-Za-z_.$?@][\w.$?@]*)",
                                 line.partition(";")[0], re.I)
    }
    embedded_spellings: dict[str, str] = {}
    for line in embedded_data:
        declaration = re.match(r"^\s*([A-Za-z_.$?@][\w.$?@]*)\s+(db|dw|dd|dq)\b",
                               line, re.I)
        if declaration:
            embedded_spellings[declaration.group(1).lower()] = declaration.group(1)
        for match in re.finditer(r"\boffset\s+([A-Za-z_.$?@][\w.$?@]*)",
                                 line.partition(";")[0], re.I):
            embedded_spellings[match.group(1).lower()] = match.group(1)
    code_source = re.sub(
        r"^(\s*)([A-Za-z_.$?@][\w.$?@]*):(?!:)",
        lambda match: match.group(0) + ":" if match.group(2).lower() in embedded_targets
        else match.group(0), code_source, flags=re.M,
    )
    for symbol in embedded_symbols:
        data_sections[symbol] = "data"
    data_names.update(embedded_symbols)

    def readable_data_lines() -> list[str]:
        lines = ["; Data islands relocated from execute-only engine text."]
        # Keep table/destination symbols in the diagnostic PIE so verification
        # can check every stored code offset. The native link hides exports.
        lines.extend(f"public {embedded_spellings[name]}"
                     for name in sorted(embedded_symbols | embedded_targets))
        for line in embedded_data:
            code, separator, comment = line.partition(";")
            def relative_code_pointer(match: re.Match[str]) -> str:
                target = match.group(1)
                if (target.lower() in proc_names
                    or data_sections.get(target.lower()) == "text"
                    or re.match(r"^(?:loc|def)_", target, re.I)):
                    return f"{target} - bedlam2_text_base"
                raise ValueError(f"Unreviewed embedded pointer: {target}")
            code = re.sub(r"\boffset\s+([A-Za-z_.$?@][\w.$?@]*)",
                          relative_code_pointer, code, flags=re.I)
            lines.append(code + (separator + comment if separator else ""))
        return lines

    for name in ("bedlam2.asm", "bedlam2.inc", "bedlam2_data.inc"):
        source = code_source if name == "bedlam2.asm" else (
            source_dir / name).read_text(errors="replace")
        output_lines: list[str] = []
        current_proc = ""
        current_data_section = ""
        in_jump_table = False
        suppress_proc_body = False
        data_pointer_fixups: list[tuple[str, str, str]] = []
        pointer_site_index = 0
        for line in source.splitlines():
            if suppress_proc_body:
                if re.match(
                    r"^\s*[A-Za-z_.$?@][\w.$?@]*\s+endp\b", line, re.I
                ):
                    output_lines.append(line)
                    current_proc = ""
                    suppress_proc_body = False
                continue
            jump_table_start = name == "bedlam2.asm" and re.match(
                r"^\s*jpt_[\w.$?@]*\s+dd\b", line, re.I
            )
            jump_table_continuation = in_jump_table and re.match(
                r"^\s*dd\b", line, re.I
            )
            proc = re.match(r"^\s*([A-Za-z_.$?@][\w.$?@]*)\s+proc\b", line, re.I)
            if proc:
                current_proc = proc.group(1).lower()
                if (
                    name == "bedlam2.asm"
                    and current_proc in PS5_RUNTIME_PROC_REPLACEMENTS
                ):
                    output_lines.append(line)
                    output_lines.extend(
                        PS5_RUNTIME_PROC_REPLACEMENTS[current_proc]
                    )
                    output_lines.append("                retn")
                    suppress_proc_body = True
                    continue
            section_directive = re.match(r"^\s*\.(data\??|code)(?=\s|$)", line, re.I)
            section_name = (
                "." + section_directive.group(1).lower()
                if section_directive else ""
            )
            if name == "bedlam2_data.inc" and section_name == ".data?":
                output_lines.extend(readable_data_lines())
                output_lines.extend((
                    "bedlam2_ps5_data_ptr_fixups label dword",
                    "public bedlam2_ps5_data_ptr_fixups",
                ))
                section_id = {"data": 0, "bss": 1, "text": 2}
                for site, target, target_section in data_pointer_fixups:
                    target_base = {
                        "data": "bedlam2_data_base",
                        "bss": "bedlam2_bss_base",
                        "text": "bedlam2_text_base",
                    }[target_section]
                    output_lines.extend((
                        f"                dd {site} - bedlam2_data_base",
                        f"                dd {target} - {target_base}",
                        f"                dd {section_id[target_section]}",
                    ))
                output_lines.extend((
                    "bedlam2_ps5_data_ptr_fixups_end label byte",
                    "public bedlam2_ps5_data_ptr_fixups_end",
                ))
                output_lines.extend((
                    "bedlam2_data_end label byte",
                    "public bedlam2_data_end",
                ))
            code_fields = line.partition(";")[0].split(None, 1)
            first_token = code_fields[0].lower() if code_fields else ""
            rewrite_data_operands = (
                name == "bedlam2.asm"
                and (bool(current_proc) or first_token in instruction_names)
            )

            line_for_transform = line
            if name == "bedlam2.inc" and re.match(
                r"^\s*extrn\s+PRESSED_KEY_ARR\s*:", line, re.I
            ):
                line_for_transform = "extrn PRESSED_KEY_ARR:qword"
            pointer_declaration = None
            if name == "bedlam2_data.inc" and current_data_section == ".data":
                pointer_declaration = re.match(
                    r"^(\s*)(?:([A-Za-z_.$?@][\w.$?@]*)\s+)?dd\s+"
                    r"offset\s+([A-Za-z_.$?@][\w.$?@]*)\s*$",
                    line.partition(";")[0],
                    re.I,
                )
            if pointer_declaration:
                indent, declared_label, target = pointer_declaration.groups()
                target_section = data_sections.get(target.lower())
                if target.lower() in proc_names:
                    target_section = "text"
                if target_section in {"data", "bss", "text"}:
                    if declared_label:
                        site = declared_label
                    else:
                        pointer_site_index += 1
                        site = f"__bedlam2_data_ptr_site_{pointer_site_index:03d}"
                        output_lines.append(f"{indent}{site} label dword")
                    data_pointer_fixups.append((site, target, target_section))
                    target_base = {
                        "data": "bedlam2_data_base",
                        "bss": "bedlam2_bss_base",
                        "text": "bedlam2_text_base",
                    }[target_section]
                    _, separator, comment = line.partition(";")
                    label_prefix = f"{declared_label} " if declared_label else indent
                    line_for_transform = (
                        f"{label_prefix}dd {target} - {target_base}"
                        + (f" ;{comment}" if separator else "")
                    )

            if name == "bedlam2.asm":
                code_pointer_declaration = re.match(
                    r"^(\s*)(?:([A-Za-z_.$?@][\w.$?@]*)\s+)?dd\s+"
                    r"offset\s+([A-Za-z_.$?@][\w.$?@]*)\s*$",
                    line.partition(";")[0],
                    re.I,
                )
                if code_pointer_declaration:
                    indent, declared_label, target = code_pointer_declaration.groups()
                    if (
                        target.lower() in proc_names
                        or re.match(r"^(?:loc|def)_", target, re.I)
                        or data_sections.get(target.lower()) == "text"
                    ):
                        label_prefix = f"{declared_label} " if declared_label else indent
                        _, separator, comment = line.partition(";")
                        line_for_transform = (
                            f"{label_prefix}dd {target} - bedlam2_text_base"
                            + (f" ;{comment}" if separator else "")
                        )

            if name == "bedlam2.asm" and re.match(
                r"^\s*byte_14723\s+db\b", line, re.I
            ):
                output_lines.append("public byte_14723")

            translated_line = transform_line(
                line_for_transform,
                proc_names,
                data_names,
                data_types,
                data_sections,
                current_proc=current_proc,
                in_jump_table=bool(jump_table_start or jump_table_continuation),
                rewrite_data_operands=rewrite_data_operands,
            )
            output_lines.extend(translated_line.split("\n"))
            if (
                name == "bedlam2.asm"
                and current_proc == "shop_room"
                and re.match(r"^\s*loc_57947\s*:", line, re.I)
            ):
                # Start steady-shop FPS timing after the room has loaded its
                # assets and finished the entry transition.
                output_lines.append(
                    "                call bedlam2_ps5_legacy_shop_loop_started"
                )
            if proc:
                output_lines.extend(
                    (
                        "                mov r14d, bedlam2_ps5_data_base_low",
                        "                mov r13d, bedlam2_ps5_bss_base_low",
                        "                lea r15, bedlam2_text_base",
                    )
                )
                if name == "bedlam2.asm" and current_proc == "main_menu":
                    # Copy the active PS5 profile name into the game's player
                    # name field before the menu starts drawing it.
                    output_lines.extend(
                        (
                            "                lea eax, [r13+player_name-bedlam2_bss_base]",
                            "                lea rsp, [rsp-4]",
                            "                mov dword ptr [rsp], 9",
                            "                lea rsp, [rsp-4]",
                            "                mov dword ptr [rsp], eax",
                            "                call bedlam2_ps5_legacy_fill_player_name",
                            "                lea rsp, [rsp+8]",
                        )
                    )
            if name == "bedlam2.asm":
                if jump_table_start or jump_table_continuation:
                    in_jump_table = True
                elif in_jump_table and (not line.strip() or line.lstrip().startswith(";")):
                    in_jump_table = True
                else:
                    in_jump_table = False
            if section_directive:
                base_label = {
                    ".data": "bedlam2_data_base",
                    ".data?": "bedlam2_bss_base",
                    ".code": "bedlam2_text_base",
                }[section_name]
                output_lines.append(f"{base_label} label byte")
                if name == "bedlam2_data.inc" and section_name == ".data":
                    output_lines.append("public bedlam2_data_base")
                elif name == "bedlam2_data.inc" and section_name == ".data?":
                    output_lines.append("public bedlam2_bss_base")
                if name == "bedlam2_data.inc":
                    current_data_section = section_name
            if current_proc and re.match(
                r"^\s*[A-Za-z_.$?@][\w.$?@]*\s+endp\b", line, re.I
            ):
                current_proc = ""
        if name == "bedlam2.inc":
            output_lines.extend(
                (
                    "extrn bedlam2_ps5_malloc:proc",
                    "extrn bedlam2_ps5_free:proc",
                    "extrn bedlam2_ps5_data_base_low:dword",
                    "extrn bedlam2_ps5_bss_base_low:dword",
                    "extrn bedlam2_ps5_symbol_search_base_low:dword",
                )
            )
            output_lines.extend(
                f"extrn bedlam2_ps5_legacy_{name}:proc"
                for name in LEGACY_EXTERNAL_THUNKS
            )
            output_lines.extend(
                (
                    "extrn bedlam2_ps5_dos_lseek_bridge:proc",
                    "extrn bedlam2_ps5_dos_qwrite_bridge:proc",
                    "extrn bedlam2_ps5_dos_isatty_bridge:proc",
                    "extrn bedlam2_ps5_keyboard_status_bridge:proc",
                )
            )
        elif name == "bedlam2_data.inc":
            output_lines.extend((
                "bedlam2_bss_end label byte",
                "public bedlam2_bss_end",
            ))
        translated = "\n".join(output_lines) + "\n"
        (output_dir / name).write_text(translated)

    object_path = output_dir / "bedlam2_x64.o"
    command = [
        str(args.jwasm.resolve()),
        "-elf64",
        "-c",
        "-zcw",
        f"-I{output_dir}",
        f"-Fo={object_path}",
        str(output_dir / "bedlam2.asm"),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        return result.returncode
    print(f"Generated experimental ELF64 object: {object_path}")
    print(f"Relocated {len(embedded_symbols)} embedded data symbols into readable engine data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
