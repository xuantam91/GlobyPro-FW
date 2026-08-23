#!/usr/bin/env python3
"""
Clean macOS junk files/folders on an SD card volume.

Typical macOS artifacts cleaned:
- ._AppleDouble sidecar files
- .DS_Store
- .Spotlight-V100, .Trashes, .fseventsd, .TemporaryItems

Usage:
  python3 scripts/clean_macos_sd_junk.py
  python3 scripts/clean_macos_sd_junk.py --dry-run
  python3 scripts/clean_macos_sd_junk.py --volume /Volumes/GLOBY16G --yes
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List


DEFAULT_VOLUMES_ROOT = Path("/Volumes")
IGNORE_VOLUME_NAMES = {"Macintosh HD"}

JUNK_FILE_NAMES = {
    ".DS_Store",
    ".localized",
    ".AppleDouble",
    ".com.apple.timemachine.supported",
    ".VolumeIcon.icns",
}

JUNK_DIR_NAMES = {
    ".Spotlight-V100",
    ".Trashes",
    ".fseventsd",
    ".TemporaryItems",
    ".DocumentRevisions-V100",
}

# Prefix artifacts usually created by Finder copy on FAT/exFAT.
JUNK_FILE_PREFIXES = ("._",)


@dataclass
class Candidate:
    path: Path
    is_dir: bool
    size_bytes: int


def human_size(size: int) -> str:
    units = ["B", "KB", "MB", "GB", "TB"]
    value = float(size)
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(value)} {unit}"
            return f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{size} B"


def collect_volumes(volumes_root: Path) -> List[Path]:
    if not volumes_root.exists():
        return []
    volumes = []
    for p in sorted(volumes_root.iterdir(), key=lambda x: x.name.lower()):
        if not p.is_dir():
            continue
        if p.name.startswith("."):
            continue
        if p.name in IGNORE_VOLUME_NAMES:
            continue
        volumes.append(p)
    return volumes


def choose_volume_interactive(volumes: List[Path]) -> Path:
    if not volumes:
        raise RuntimeError("Không tìm thấy volume nào trong /Volumes.")

    print("Chọn thẻ nhớ cần dọn:")
    for i, vol in enumerate(volumes, start=1):
        print(f"  {i}. {vol}")

    while True:
        raw = input("Nhập số: ").strip()
        try:
            idx = int(raw)
            if 1 <= idx <= len(volumes):
                return volumes[idx - 1]
        except ValueError:
            pass
        print("Lựa chọn không hợp lệ, nhập lại.")


def dir_size(path: Path) -> int:
    total = 0
    for root, dirs, files in os.walk(path, topdown=True, followlinks=False):
        dirs[:] = [d for d in dirs if not Path(root, d).is_symlink()]
        for f in files:
            fp = Path(root, f)
            try:
                if not fp.is_symlink():
                    total += fp.stat().st_size
            except OSError:
                continue
    return total


def is_junk_file(name: str, all_dotfiles: bool) -> bool:
    if name in JUNK_FILE_NAMES:
        return True
    if any(name.startswith(prefix) for prefix in JUNK_FILE_PREFIXES):
        return True
    if all_dotfiles and name.startswith("."):
        return True
    return False


def is_junk_dir(name: str, all_dotfiles: bool) -> bool:
    if name in JUNK_DIR_NAMES:
        return True
    if all_dotfiles and name.startswith("."):
        return True
    return False


def scan_candidates(volume: Path, all_dotfiles: bool) -> List[Candidate]:
    candidates: List[Candidate] = []
    for root, dirs, files in os.walk(volume, topdown=True, followlinks=False):
        root_path = Path(root)

        # Remove junk dirs from traversal and schedule deletion.
        to_prune = []
        for d in list(dirs):
            if is_junk_dir(d, all_dotfiles):
                dp = root_path / d
                size = dir_size(dp)
                candidates.append(Candidate(path=dp, is_dir=True, size_bytes=size))
                to_prune.append(d)
        for d in to_prune:
            dirs.remove(d)

        for f in files:
            if is_junk_file(f, all_dotfiles):
                fp = root_path / f
                try:
                    size = fp.stat().st_size
                except OSError:
                    size = 0
                candidates.append(Candidate(path=fp, is_dir=False, size_bytes=size))
    return candidates


def delete_candidates(candidates: List[Candidate], dry_run: bool) -> tuple[int, int, int, int]:
    deleted_count = 0
    freed = 0
    failed = 0
    skipped = 0
    for c in candidates:
        rel = str(c.path)
        if dry_run:
            print(f"[DRY-RUN] {'DIR ' if c.is_dir else 'FILE'} {rel}")
            deleted_count += 1
            freed += c.size_bytes
            continue
        try:
            if c.is_dir:
                shutil.rmtree(c.path)
            else:
                c.path.unlink(missing_ok=True)
            print(f"[OK] {'DIR ' if c.is_dir else 'FILE'} {rel}")
            deleted_count += 1
            freed += c.size_bytes
        except PermissionError:
            # Some system-managed folders on FAT volumes can be protected by macOS.
            skipped += 1
            print(f"[SKIP] {rel} (permission denied by system)")
        except Exception as exc:  # pylint: disable=broad-except
            failed += 1
            print(f"[ERR] {rel} -> {exc}")
    return deleted_count, freed, failed, skipped


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Dọn file rác macOS trên thẻ nhớ (SD/USB)."
    )
    parser.add_argument(
        "--volume",
        type=Path,
        help="Đường dẫn volume, ví dụ: /Volumes/GLOBY16G",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Chỉ in danh sách sẽ xóa, không xóa thật.",
    )
    parser.add_argument(
        "--all-dotfiles",
        action="store_true",
        help="Xóa tất cả file/thư mục bắt đầu bằng dấu chấm (nguy hiểm hơn).",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Không hỏi xác nhận trước khi xóa.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.volume is None:
        volumes = collect_volumes(DEFAULT_VOLUMES_ROOT)
        try:
            volume = choose_volume_interactive(volumes)
        except RuntimeError as exc:
            print(exc)
            return 1
    else:
        volume = args.volume.expanduser().resolve()

    if not volume.exists() or not volume.is_dir():
        print(f"Volume không hợp lệ: {volume}")
        return 1

    print(f"Volume đã chọn: {volume}")
    if args.all_dotfiles:
        print("Mode: all-dotfiles (xóa mọi file/folder bắt đầu bằng '.')")
    else:
        print("Mode: macOS junk an toàn")

    candidates = scan_candidates(volume, args.all_dotfiles)
    total_bytes = sum(c.size_bytes for c in candidates)
    print(f"Tìm thấy {len(candidates)} mục rác, dung lượng ~ {human_size(total_bytes)}")

    if not candidates:
        print("Không có gì để dọn.")
        return 0

    if not args.yes and not args.dry_run:
        ans = input("Xác nhận xóa? [y/N]: ").strip().lower()
        if ans not in {"y", "yes"}:
            print("Đã hủy.")
            return 0

    deleted_count, freed, failed, skipped = delete_candidates(candidates, args.dry_run)
    mode = "DRY-RUN" if args.dry_run else "DONE"
    print(
        f"[{mode}] Đã xử lý {deleted_count} mục, giải phóng ~ {human_size(freed)}, "
        f"skip {skipped} mục, lỗi {failed} mục."
    )
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
