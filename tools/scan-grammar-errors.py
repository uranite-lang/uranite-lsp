#!/usr/bin/env python3

import subprocess
import sys
import os
from pathlib import Path

TSITTER_DIR = os.environ.get(
    "URANITE_TSITTER_DIR",
    str(Path(__file__).resolve().parent.parent.parent / "uranite-tsitter")
)

STDLIBS_DIR = os.environ.get(
    "URANITE_STDLIBS_DIR",
    str(Path(__file__).resolve().parent.parent.parent / "uranite" / "stdlibs")
)

def scan_file(filepath):
    try:
        result = subprocess.run(
            ["npx", "tree-sitter", "parse", str(filepath)],
            capture_output=True, text=True, timeout=30,
            cwd=TSITTER_DIR
        )
        output = result.stdout + result.stderr
        errors = []
        for line in output.splitlines():
            if "ERROR" in line or "MISSING" in line:
                errors.append(line.strip())
        return errors
    except subprocess.TimeoutExpired:
        return ["TIMEOUT: parse took >30s"]
    except Exception as e:
        return [f"EXCEPTION: {e}"]

def main():
    scan_dir = sys.argv[1] if len(sys.argv) > 1 else STDLIBS_DIR

    if not os.path.isdir(scan_dir):
        print(f"Directory not found: {scan_dir}")
        sys.exit(1)

    urn_files = sorted(Path(scan_dir).rglob("*.urn"))
    total = len(urn_files)
    clean = 0
    broken = 0
    results = []

    print(f"Scanning {total} .urn files in {scan_dir}")
    print(f"Using tree-sitter from {TSITTER_DIR}")
    print()

    for i, filepath in enumerate(urn_files, 1):
        rel = os.path.relpath(filepath, scan_dir)
        errors = scan_file(filepath)
        if errors:
            broken += 1
            results.append((rel, errors))
            print(f"  [{i}/{total}] FAIL  {rel} ({len(errors)} error nodes)")
        else:
            clean += 1
            print(f"  [{i}/{total}] OK    {rel}")

    print()
    print(f"Results: {clean} clean, {broken} with errors, {total} total")

    if results:
        print()
        print("=" * 70)
        print("FILES WITH ERRORS")
        print("=" * 70)
        for rel, errors in results:
            print(f"\n--- {rel} ---")
            for err in errors:
                print(f"  {err}")

    sys.exit(1 if broken else 0)

if __name__ == "__main__":
    main()
