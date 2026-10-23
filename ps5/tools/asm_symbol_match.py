#!/usr/bin/env python3
"""Match assembled source procedures to function starts in a DOS/4GW image.

The source listing and shipped executable use different layouts and data
addresses. This tool compares normalized x86 instruction signatures so source
names can be mapped back to the original image for reverse-engineering work.
It reports candidates only; it does not modify either binary or any source.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG


PROC_RE = re.compile(r"^\s*([\w?$@.]+)\s+proc\b", re.IGNORECASE | re.MULTILINE)
NM_RE = re.compile(r"^([0-9a-fA-F]+)\s+[Tt]\s+(\S+)$")
RELOC_RE = re.compile(r"^\s*([0-9a-fA-F]+)\s+\S+\s+R_386_(?:32|PC32)\b")


def command_output(*argv: str) -> str:
    return subprocess.check_output(argv, text=True)


def elf_symbols(object_path: Path) -> dict[str, int]:
    symbols = {}
    for line in command_output("nm", "-n", str(object_path)).splitlines():
        match = NM_RE.match(line)
        if match:
            symbols[match.group(2).casefold()] = int(match.group(1), 16)
    return symbols


def relocation_bytes(object_path: Path) -> set[int]:
    offsets: set[int] = set()
    for line in command_output("readelf", "-r", str(object_path)).splitlines():
        match = RELOC_RE.match(line)
        if match:
            start = int(match.group(1), 16)
            offsets.update(range(start, start + 4))
    return offsets


def object_text(object_path: Path) -> bytes:
    return subprocess.check_output(
        ["objcopy", "-O", "binary", "--only-section=.text", str(object_path), "/dev/stdout"]
    )


def signature(
    decoder: capstone.Cs,
    raw: bytes,
    address: int,
    relocation_start: int,
    relocation_offsets: set[int],
    instruction_limit: int,
) -> tuple[tuple[object, ...], ...]:
    result: list[tuple[object, ...]] = []
    for instruction in decoder.disasm(raw, address):
        normalized_operands = []
        for operand in instruction.operands:
            if operand.type == X86_OP_REG:
                normalized_operands.append(("reg", decoder.reg_name(operand.reg), operand.size))
            elif operand.type == X86_OP_IMM:
                immediate_start = relocation_start + instruction.address - address + instruction.encoding.imm_offset
                immediate_end = immediate_start + instruction.encoding.imm_size
                is_relocated = bool(instruction.encoding.imm_offset) and any(
                    byte in relocation_offsets for byte in range(immediate_start, immediate_end)
                )
                if instruction.mnemonic == "call":
                    value = ("call-target",)
                elif instruction.mnemonic.startswith(("j", "loop")):
                    value = ("branch-target", operand.imm - address)
                elif is_relocated:
                    value = ("relocated-immediate",)
                else:
                    value = ("immediate", operand.imm, operand.size)
                normalized_operands.append(value)
            elif operand.type == X86_OP_MEM:
                displacement_start = relocation_start + instruction.address - address + instruction.encoding.disp_offset
                displacement_end = displacement_start + instruction.encoding.disp_size
                is_relocated = bool(instruction.encoding.disp_offset) and any(
                    byte in relocation_offsets for byte in range(displacement_start, displacement_end)
                )
                memory = operand.mem
                normalized_operands.append(
                    (
                        "memory",
                        operand.size,
                        decoder.reg_name(memory.segment) if memory.segment else "",
                        decoder.reg_name(memory.base) if memory.base else "",
                        decoder.reg_name(memory.index) if memory.index else "",
                        memory.scale,
                        None if is_relocated else memory.disp,
                    )
                )
            else:
                normalized_operands.append(("other", operand.size))
        result.append((instruction.mnemonic, tuple(normalized_operands)))
        if len(result) >= instruction_limit or instruction.mnemonic.startswith("ret"):
            break
    return tuple(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asm", type=Path, default=Path("ASM_Sources/bedlam2.asm"))
    parser.add_argument("--object", type=Path, required=True, help="ELF32 object assembled from --asm")
    parser.add_argument("--le-image", type=Path, required=True, help="flat image emitted by an LE parser")
    parser.add_argument("--le-json", type=Path, required=True, help="LE metadata emitted with --le-image")
    parser.add_argument("--functions-json", type=Path, required=True, help="candidate catalog of image function starts")
    parser.add_argument("--instructions", type=int, default=24, help="maximum instruction prefix to compare")
    parser.add_argument("--output", type=Path, help="optional JSON mapping output")
    args = parser.parse_args()

    procedures = [name.casefold() for name in PROC_RE.findall(args.asm.read_text(errors="replace"))]
    symbols = elf_symbols(args.object)
    source_code = object_text(args.object)
    source_relocations = relocation_bytes(args.object)
    image = args.le_image.read_bytes()
    metadata = json.loads(args.le_json.read_text())
    image_base = int(metadata["image_base"])
    fixup_bytes: set[int] = set()
    for fixup in metadata["fixups"]:
        if fixup["type"] in (6, 7, 8):
            start = int(fixup["at"]) - image_base
            fixup_bytes.update(range(start, start + 4))

    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    code_end = max(
        int(obj["addr"]) + int(obj["vsize"])
        for obj in metadata["objects"]
        if int(obj["flags"]) & 4
    )
    catalog = json.loads(args.functions_json.read_text())
    candidates = sorted(
        {
            int(entry["address"])
            for entry in catalog["functions"]
            if image_base <= int(entry["address"]) < code_end
            and entry.get("entry_kind") != "alias"
        }
    )
    target_signatures = {
        address: signature(
            decoder,
            image[address - image_base : min(code_end, address + 15 * args.instructions) - image_base],
            address,
            address - image_base,
            fixup_bytes,
            args.instructions,
        )
        for address in candidates
    }

    mappings = []
    for name in procedures:
        object_address = symbols.get(name)
        if object_address is None:
            mappings.append({"name": name, "status": "missing-object-symbol"})
            continue
        source_signature = signature(
            decoder,
            source_code[object_address : object_address + 15 * args.instructions],
            object_address,
            object_address,
            source_relocations,
            args.instructions,
        )
        matches = [
            address
            for address, candidate_signature in target_signatures.items()
            if len(candidate_signature) >= len(source_signature)
            and candidate_signature[: len(source_signature)] == source_signature
        ]
        status = "unique" if len(matches) == 1 else "ambiguous" if matches else "unmatched"
        mappings.append(
            {
                "name": name,
                "object_address": object_address,
                "signature_instructions": len(source_signature),
                "status": status,
                "image_addresses": matches,
            }
        )

    counts = {status: sum(item.get("status") == status for item in mappings)
              for status in ("unique", "ambiguous", "unmatched", "missing-object-symbol")}
    print(f"Procedures: {len(procedures)}; candidates: {len(candidates)}")
    print("Matches: " + ", ".join(f"{key}={value}" for key, value in counts.items()))
    for item in mappings:
        if item.get("status") == "unique":
            print(f"{item['name']}: 0x{item['image_addresses'][0]:08X} ({item['signature_instructions']} instructions)")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"counts": counts, "mappings": mappings}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
