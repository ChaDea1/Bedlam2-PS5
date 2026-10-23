#!/usr/bin/env python3
"""Conservatively summarize live procedures in the Bedlam 2 assembly source.

This is a review aid, not a dead-code remover. It follows direct procedure calls
and treats any procedure whose address is referenced as an additional root.
Indirect calls and data-driven control flow still need manual review.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass, field
from pathlib import Path


PROC_RE = re.compile(r"^\s*([\w?$@.]+)\s+proc\b", re.IGNORECASE)
ENDP_RE = re.compile(r"^\s*[\w?$@.]+\s+endp\b", re.IGNORECASE)
CALL_RE = re.compile(
    r"\bcall\s+(?:(?:near|dword|qword|fword)\s+ptr\s+)?([^\s,;]+)",
    re.IGNORECASE,
)
OFFSET_RE = re.compile(r"\boffset\s+([\w?$@.]+)", re.IGNORECASE)
DATA_POINTER_RE = re.compile(r"^\s*(?:dd|dq)\s+([\w?$@.]+)\b", re.IGNORECASE)
INTERRUPT_RE = re.compile(r"\bint\s+(?:21h|31h)\b", re.IGNORECASE)


@dataclass
class Procedure:
    name: str
    start_line: int
    body: list[tuple[int, str]] = field(default_factory=list)
    calls: set[str] = field(default_factory=set)
    dos_interrupts: list[tuple[int, str]] = field(default_factory=list)


def code_part(line: str) -> str:
    return line.split(";", 1)[0]


def parse_procedures(path: Path) -> dict[str, Procedure]:
    procedures: dict[str, Procedure] = {}
    active: Procedure | None = None
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        match = PROC_RE.match(line)
        if match:
            name = match.group(1)
            key = name.casefold()
            active = Procedure(name=name, start_line=line_number)
            if key in procedures:
                raise ValueError(f"duplicate procedure name at {path}:{line_number}: {name}")
            procedures[key] = active
        if active is not None:
            active.body.append((line_number, line))
            code = code_part(line)
            if INTERRUPT_RE.search(code):
                active.dos_interrupts.append((line_number, code.strip()))
            if ENDP_RE.match(line):
                active = None
    if active is not None:
        raise ValueError(f"unterminated procedure: {active.name}")
    return procedures


def procedure_references(path: Path, procedure_names: dict[str, str]) -> dict[str, list[tuple[int, str]]]:
    references: dict[str, list[tuple[int, str]]] = {}
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        code = code_part(line)
        match = OFFSET_RE.search(code)
        if match:
            key = match.group(1).casefold()
            if key in procedure_names:
                references.setdefault(key, []).append((line_number, line.strip()))
            continue
        match = DATA_POINTER_RE.match(code)
        if match:
            key = match.group(1).casefold()
            if key in procedure_names:
                references.setdefault(key, []).append((line_number, line.strip()))
    return references


def resolve_calls(procedures: dict[str, Procedure]) -> None:
    procedure_names = {key: procedure.name for key, procedure in procedures.items()}
    for procedure in procedures.values():
        for _, line in procedure.body:
            match = CALL_RE.search(code_part(line))
            if match:
                target = match.group(1).strip("[]").casefold()
                if target in procedure_names:
                    procedure.calls.add(target)


def reachable_from(roots: set[str], procedures: dict[str, Procedure]) -> set[str]:
    reachable: set[str] = set()
    pending = list(roots)
    while pending:
        current = pending.pop()
        if current in reachable or current not in procedures:
            continue
        reachable.add(current)
        pending.extend(procedures[current].calls - reachable)
    return reachable


def indirect_calls(
    procedures: dict[str, Procedure], reachable: set[str]
) -> list[tuple[str, int, str]]:
    result = []
    for key in sorted(reachable):
        procedure = procedures[key]
        for line_number, line in procedure.body:
            code = code_part(line)
            match = CALL_RE.search(code)
            if not match:
                continue
            operand = match.group(1).casefold()
            if is_indirect_operand(operand):
                result.append((procedure.name, line_number, code.strip()))
    return result


def is_indirect_operand(operand: str) -> bool:
    return "[" in operand or operand.startswith(("dword_", "qword_", "fword_", "off_", "funcs_"))


def external_call_targets(
    procedures: dict[str, Procedure], reachable: set[str]
) -> set[str]:
    targets = set()
    for key in reachable:
        for _, line in procedures[key].body:
            match = CALL_RE.search(code_part(line))
            if not match:
                continue
            target = match.group(1).casefold()
            if target not in procedures and not is_indirect_operand(target):
                targets.add(target)
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asm", type=Path, default=Path("ASM_Sources/bedlam2.asm"))
    parser.add_argument("--data", type=Path, default=Path("ASM_Sources/bedlam2_data.inc"))
    parser.add_argument("--details", action="store_true", help="list DOS interrupt procedures and indirect calls")
    args = parser.parse_args()

    procedures = parse_procedures(args.asm)
    procedure_names = {key: procedure.name for key, procedure in procedures.items()}
    resolve_calls(procedures)
    address_refs = procedure_references(args.asm, procedure_names)
    for key, refs in procedure_references(args.data, procedure_names).items():
        address_refs.setdefault(key, []).extend(refs)

    entry = "main_"
    entry_key = entry.casefold()
    if entry_key not in procedures:
        parser.error(f"entry procedure {entry!r} was not found")
    direct = reachable_from({entry_key}, procedures)
    roots = {entry_key, *address_refs}
    reachable = reachable_from(roots, procedures)
    dos_procedures = [procedures[key] for key in sorted(reachable) if procedures[key].dos_interrupts]
    indirect = indirect_calls(procedures, reachable)
    external_targets = external_call_targets(procedures, reachable)

    print(f"Procedures: {len(procedures)}")
    print(f"Direct-call reachable from {entry}: {len(direct)}")
    print(f"Address-taken procedure roots: {len(address_refs)}")
    print(f"Expanded conservative reachability: {len(reachable)}")
    print(f"Reachable procedures with INT 21h/31h: {len(dos_procedures)}")
    print(f"Reachable indirect call sites: {len(indirect)}")
    print(f"External direct-call targets: {len(external_targets)}")
    if args.details:
        for procedure in dos_procedures:
            for line_number, instruction in procedure.dos_interrupts:
                print(f"DOS {procedure.name} {line_number}: {instruction}")
        for name, line_number, instruction in indirect:
            print(f"INDIRECT {name} {line_number}: {instruction}")
        for target in sorted(external_targets):
            print(f"EXTERNAL {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
