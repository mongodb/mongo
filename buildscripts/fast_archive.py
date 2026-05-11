import argparse
import concurrent.futures
import glob
import gzip
import os
import shutil
import subprocess
import sys
import time
from typing import Optional


def process_file(file: str, start_time: float) -> Optional[str]:
    print(f"{file} started compressing at {time.time() - start_time}")
    compressed_file = f"{file}.gz"
    with open(file, "rb") as f_in:
        with gzip.open(compressed_file, "wb") as f_out:
            shutil.copyfileobj(f_in, f_out)

    print(f"{file} finished compressing at {time.time() - start_time}")
    return compressed_file


def main(patterns: list[str]) -> int:
    start_time = time.time()
    files: set[str] = set()

    for pattern in patterns:
        glob_results = glob.glob(pattern)
        for path in glob_results:
            # Try the path as-is first (works on Linux/Mac with native symlinks)
            if os.path.isfile(path):
                files.add(path)
            # On Windows, Cygwin symlinks are not recognized by Python
            # Use cygpath to resolve the symlink and convert to Windows path
            elif sys.platform in ("win32", "cygwin"):
                try:
                    result = subprocess.run(
                        ["bash", "-c", f'cygpath -wa "{path}"'],
                        capture_output=True,
                        text=True,
                        timeout=5,
                    )
                    if result.returncode == 0:
                        resolved = result.stdout.strip()
                        if resolved and os.path.isfile(resolved):
                            files.add(resolved)
                    else:
                        print(
                            f"ERROR: cygpath command failed for {path}: {result.stderr}",
                            file=sys.stderr,
                        )
                except Exception as e:
                    print(f"ERROR: Could not resolve symlink {path}: {e}", file=sys.stderr)

    file_list = list(files)

    if not file_list:
        print("No files found for the input, exiting early")
        return 0

    cores = os.cpu_count()
    with concurrent.futures.ThreadPoolExecutor(max_workers=cores) as executor:
        futures = [
            executor.submit(
                process_file,
                file=path,
                start_time=start_time,
            )
            for path in file_list
        ]
        for future in concurrent.futures.as_completed(futures):
            future.result()

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog="FastArchiver",
        description="Compresses files in parallel using gzip for subsequent upload via Evergreen's "
        "s3.put command.",
    )

    parser.add_argument(
        "--pattern",
        "-p",
        help="glob patterns of files to be archived.",
        dest="patterns",
        action="append",
        default=[],
        required=True,
    )
    args = parser.parse_args()
    exit(main(args.patterns))
