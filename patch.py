#!/usr/bin/env python3
"""
Patch binaries for Tencent Wemeet v3.26.10.401 Linux version.
Includes SHA256 verification to ensure offset safety.
"""

import mmap
import os
import sys
import argparse
import hashlib
from typing import Dict, List, Tuple

Hunk = Dict[int, Tuple[bytes, bytes]]

EXPECTED_HASHES = {
    "bin/modules/screen_share/libscreen_share_module.so": "925e7c16a5d69adaa804d1ea55aac1edc4093aacb5324948ea6f38a71abd4e16",
}

PATCHES: Dict[str, List[Hunk]] = {
    "bin/modules/screen_share/libscreen_share_module.so": [
    { # Hunk 0: move `dbusStart()` from `dbusSelectSources()` to `onSelectSourcesResponse()`
        0x43d2fa: (
            bytes.fromhex("00"),
            bytes.fromhex("5d")),
        0x43d331: (
            bytes.fromhex("88f7"),
            bytes.fromhex("60fb")),
        0x43d336: (
            bytes.fromhex("83c718488b8588f7ffff48"),
            bytes.fromhex("89fe4883c618e910000000")),
        0x43d357: (
            bytes.fromhex("0000"),
            bytes.fromhex("e208")),
        0x43dc38: (
            bytes.fromhex("e8c305d9"),
            bytes.fromhex("e9ecf6ff")),
    }, { # Hunk 1: color format negotiation spoofing (08 -> 07)
        0x450fb6: (
            bytes.fromhex("08"),
            bytes.fromhex("07")),
        0x4566e4: (
            bytes.fromhex("8b45c08b4808"),
            bytes.fromhex("c7c108000000")),
    }],
}

# ── Hunk-level operations ─────────────────────────────────────────────────────

def validate_hunk(mm: mmap.mmap, hunk: Hunk, file_size: int, show_ok: bool = True) -> bool:
    ok = True
    for offset, (original, new) in sorted(hunk.items()):
        if new is None:
            new = original
        end = offset + max(len(original), len(new))
        if end > file_size:
            print(f"    [BOUNDS]   0x{offset:08x}: end 0x{end:08x} exceeds file size 0x{file_size:08x}")
            ok = False
            break
        mm.seek(offset)
        actual = mm.read(len(original))
        if actual != original:
            print(f"    [MISMATCH] 0x{offset:08x}: expected {original.hex()}  got {actual.hex()}")
            ok = False
            break
        elif show_ok:
            print(f"    [OK]       0x{offset:08x}: {original.hex()}")
    return ok


def apply_hunk(mm: mmap.mmap, hunk: Hunk) -> int:
    count = 0
    for offset, (_original, new) in sorted(hunk.items()):
        if new is None:
            continue
        l = len(new)
        print(f"    [PATCH]    0x{offset:08x}: {mm[offset:offset+l].hex()} => {new.hex()}")
        mm.seek(offset)
        mm.write(new)
        count += l
    return count


def verify_file_hash(path: str, expected_hash: str) -> bool:
    sha256 = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256.update(block)
    actual_hash = sha256.hexdigest()
    if actual_hash != expected_hash:
        print(f"\n[WARNING] Hash mismatch for {path}!")
        print(f"          Expected: {expected_hash}")
        print(f"          Actual  : {actual_hash}")
        print("          The file version might have changed. Hardcoded patch offsets may corrupt the binary.")
        print("          Applying patches anyway, but stability is not guaranteed.\n")
        return False
    else:
        print(f"  Hash verified successfully.")
        return True


# ── File-level orchestration ──────────────────────────────────────────────────

def patch_file(path: str, rel_path: str, hunks: List[Hunk], dry_run: bool = False) -> int:
    if not os.path.isfile(path):
        print(f"[ERROR] File not found: {path}")
        return 2

    if rel_path in EXPECTED_HASHES:
        verify_file_hash(path, EXPECTED_HASHES[rel_path])

    file_size = os.path.getsize(path)
    size_applied = 0
    n_applied = 0
    n_rejected = 0

    access = mmap.ACCESS_READ if dry_run else mmap.ACCESS_WRITE
    with open(path, "r+b" if not dry_run else "rb") as fh:
        with mmap.mmap(fh.fileno(), 0, access=access) as mm:
            for i, hunk in enumerate(hunks):
                if not hunk:
                    continue
                print(f"\n── Hunk #{i} ({'%d site%s' % (len(hunk), 's' if len(hunk) != 1 else '')}) {'─' * 40}")
                if validate_hunk(mm, hunk, file_size, dry_run):
                    if not dry_run:
                        n = apply_hunk(mm, hunk)
                        print(f"  → Hunk #{i} applied ({n} byte(s))")
                        size_applied += n
                        n_applied += 1
                else:
                    print(f"  → Hunk #{i} rejected")
                    n_rejected += 1

            if size_applied != 0:
                mm.flush()

    print(f"\n── Summary {'─' * 50}")
    if dry_run:
        print(f"  dry-run: {len(hunks) - n_rejected} would apply, {n_rejected} rejected")
    else:
        print(f"  applied: {n_applied}  rejected: {n_rejected}\n  bytes written: {size_applied}")

    return 0 if n_rejected == 0 else 1


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Patch binaries for Tencent Wemeet v3.26.10.401 Linux version."
    )
    parser.add_argument("install_dir", help="Tencent Wemeet installation directory (e.g. /opt/wemeet)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate only, do not write anything")
    args = parser.parse_args()

    install_dir = args.install_dir.rstrip("/")
    overall_ok = True

    for rel_path, hunks in PATCHES.items():
        full_path = f"{install_dir}/{rel_path}"
        print(f"\n{'═' * 64}")
        print(f"  Target : {full_path}")
        print(f"  Hunks  : {len(hunks)}")

        rc = patch_file(full_path, rel_path, hunks, dry_run=args.dry_run)
        if rc != 0:
            overall_ok = False

        print(f"{'═' * 64}")

    sys.exit(0 if overall_ok else 1)


if __name__ == "__main__":
    main()
