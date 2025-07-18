#!/usr/bin/env python3

import hashlib
import os
import sys


def compute_sha256(file_path: str) -> str:
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256.update(block)
    return sha256.hexdigest()

def write_sha256_file(file_path: str, hash_value: str):
    sha256_path = file_path + ".sha256"
    file_name = os.path.basename(file_path)
    with open(sha256_path, "w") as f:
        f.write(f"{hash_value}  {file_name}\n")
    print(f"Wrote SHA-256 to {sha256_path}")

def main():
    if len(sys.argv) != 2:
        print("Usage: sha256sum.py <file>")
        sys.exit(1)

    file_path = sys.argv[1]
    if not os.path.isfile(file_path):
        print(f"Error: '{file_path}' is not a valid file.")
        sys.exit(1)

    hash_value = compute_sha256(file_path)
    write_sha256_file(file_path, hash_value)

if __name__ == "__main__":
    main()
