#!/usr/bin/env python3
"""Count x86-32 compatibility constructs in Bedlam 2's live assembly.

This is a planning aid for the native engine port, not a source transformer.
It scopes results to the conservative reachability set from asm_reachability.py.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

import asm_reachability as ar


@dataclass(frozen=True)
class Feature:
    label: str
    pattern: re.Pattern[str]


FEATURES = (
    Feature("PUSHA/POPA (not available in long mode)", re.compile(r"\b(?:pusha|popa|pushad|popad)\b", re.I)),
    Feature("explicit ESP-relative memory operands", re.compile(r"\[\s*esp(?:\s*[+\-])", re.I)),
    Feature("segment-register operands or overrides", re.compile(r"\b(?:cs|ds|es|fs|gs|ss)\s*:", re.I)),
    Feature("32-bit register-based memory operands", re.compile(r"\[(?:esp|ebp|esi|edi|eax|ebx|ecx|edx)\b", re.I)),
    Feature("OFFSET address expressions", re.compile(r"\boffset\s+", re.I)),
    Feature("DD data declarations in procedures", re.compile(r"^\s*dd\b", re.I)),
)


def live_procedures(asm_path: Path, data_path: Path) -> tuple[dict[str, ar.Procedure], set[str]]:
    procedures = ar.parse_procedures(asm_path)
    ar.resolve_calls(procedures)
    names = {key: procedure.name for key, procedure in procedures.items()}
    references = ar.procedure_references(asm_path, names)
    for key, refs in ar.procedure_references(data_path, names).items():
        references.setdefault(key, []).extend(refs)
    roots = {"main_", *references}
    return procedures, ar.reachable_from(roots, procedures)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asm", type=Path, default=Path("ASM_Sources/bedlam2.asm"))
    parser.add_argument("--data", type=Path, default=Path("ASM_Sources/bedlam2_data.inc"))
    args = parser.parse_args()

    procedures, reachable = live_procedures(args.asm, args.data)
    print(f"Scope: {len(reachable)} conservatively reachable procedures")
    for feature in FEATURES:
        count = 0
        affected = 0
        for key in reachable:
            matched = any(
                feature.pattern.search(ar.code_part(line))
                for _, line in procedures[key].body
            )
            if matched:
                affected += 1
            for _, line in procedures[key].body:
                if feature.pattern.search(ar.code_part(line)):
                    count += 1
        print(f"{feature.label}: {count} lines in {affected} procedures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
