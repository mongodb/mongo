#!/usr/bin/env python3

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import time
import traceback

sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from buildscripts.s3_binary.hashes import S3_SHA256_HASHES
from buildscripts.util.download_utils import (
    download_from_s3_with_boto,
    download_from_s3_with_requests,
)


def read_sha_file(filename):
    with open(filename) as f:
        content = f.read()
        return content.strip().split()[0]

def _fetch_remote_sha256_hash(s3_path: str):
    downloaded = False
    result = None
    tempfile_name = None
    with tempfile.NamedTemporaryFile(delete=False) as temp_file:
        tempfile_name = temp_file.name
        try:
            download_from_s3_with_boto(s3_path + ".sha256", temp_file.name)
            downloaded = True
        except Exception:
            try:
                download_from_s3_with_requests(s3_path + ".sha256", temp_file.name)
                downloaded = True
            except Exception:
                pass

    if downloaded:
        result = read_sha_file(tempfile_name)
    
    if tempfile_name and os.path.exists(tempfile_name):
        os.unlink(tempfile_name)

    return result


def _sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


def _verify_s3_hash(s3_path: str, local_path: str, expected_hash: str) -> None:
    hash_string = _sha256_file(local_path)
    if hash_string != expected_hash:
        raise ValueError(
            f"Hash mismatch for {s3_path}, expected {expected_hash} but got {hash_string}"
        )

def validate_file(s3_path, output_path, remote_sha_allowed):
    hexdigest = S3_SHA256_HASHES.get(s3_path)
    if hexdigest:
        print(f"Validating against hard coded sha256: {hexdigest}")
        _verify_s3_hash(s3_path, output_path, hexdigest)
        return True
    
    if not remote_sha_allowed:
        raise ValueError(f"No SHA256 hash available for {s3_path}")

    if os.path.exists(output_path + ".sha256"):
        hexdigest = read_sha_file(output_path + ".sha256")
        print(f"Validating against sh256 file {hexdigest}\n{output_path}.sha256")
    else:
        hexdigest = _fetch_remote_sha256_hash(s3_path)
        if hexdigest:
            print(f"Validating against remote sha256 {hexdigest}\n({s3_path}.sha256)")
        else:
            print(f"Failed to download remote sha256 at {s3_path}.sha256)")
            
    if hexdigest:
        _verify_s3_hash(s3_path, output_path, hexdigest)
        return True
    else:
        raise ValueError(f"No SHA256 hash available for {s3_path}")
        

def _download_and_verify(s3_path, output_path, remote_sha_allowed):
    for i in range(5):
        try:
            print(f"Downloading {s3_path}...")
            try:
                download_from_s3_with_boto(s3_path, output_path)
            except Exception:
                download_from_s3_with_requests(s3_path, output_path)
                
            validate_file(s3_path, output_path, remote_sha_allowed)
            break

        except Exception:
            print("Download failed:")
            traceback.print_exc()
            if i == 4:
                raise
            print("Retrying download...")
            time.sleep(3)
            continue


def download_s3_binary(
    s3_path: str,
    local_path: str = None,
    remote_sha_allowed=False,
) -> bool:
    if local_path is None:
        local_path = s3_path.split("/")[-1]

    if os.path.exists(local_path):
        try:
            print(f"Downloaded file {local_path} already exists, validating...")
            validate_file(s3_path, local_path, remote_sha_allowed)
            return True
        except Exception:
            print("File is invalid, redownloading...")

    tempfile_name = None
    try:
        with tempfile.NamedTemporaryFile(delete=False) as temp_file:
            tempfile_name = temp_file.name
            _download_and_verify(s3_path, tempfile_name, remote_sha_allowed)

        try:
            os.replace(tempfile_name, local_path)
        except OSError as e:
            if e.errno == 18:  # EXDEV cross filesystem error, need to use a mv
                shutil.move(tempfile_name, local_path)
            else:
                raise

        print(f"Downloaded and verified {s3_path} -> {local_path}")
        return True
    except Exception as e:
        print(f"Download failed for {s3_path}: {e}")
        traceback.print_exc()
        return False
    finally:
        if tempfile_name and os.path.exists(tempfile_name):
            os.unlink(tempfile_name)


if __name__ == "__main__":

    parser = argparse.ArgumentParser(description="Download and verify S3 binary.")
    parser.add_argument("s3_path", help="S3 URL to download from")
    parser.add_argument("local_path", nargs="?", help="Optional output file path")
    parser.add_argument("--remote-sha", action="store_true", help="Allow remote .sha256 lookup")

    args = parser.parse_args()

    if not download_s3_binary(args.s3_path, args.local_path, args.remote_sha):
        sys.exit(1)
