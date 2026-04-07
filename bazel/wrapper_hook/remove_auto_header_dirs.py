import os
import sys
import shutil
from concurrent.futures import ThreadPoolExecutor

TARGET = ".auto_header"


def delete_dir(path: str):
    # Don't follow/delete symlinked dirs
    if os.path.islink(path):
        return False
    try:
        shutil.rmtree(path, ignore_errors=True)
        return True
    except Exception:
        return False


def find_targets(root: str):
    for dirpath, dirnames, _ in os.walk(root):
        # If TARGET exists directly under this directory
        if TARGET in dirnames:
            target_path = os.path.join(dirpath, TARGET)
            yield target_path
            # Prevent os.walk from descending into it
            dirnames.remove(TARGET)


def clean_up_auto_header_dirs(ROOT):
    targets = list(find_targets(ROOT))
    if not targets:
        return

    workers = min(32, (os.cpu_count() or 8) * 2)
    with ThreadPoolExecutor(max_workers=workers) as ex:
        _ = list(ex.map(delete_dir, targets))
