#!/usr/bin/env python3

import hashlib
import os
import shutil
import tempfile
import time
import urllib.request

from buildscripts.s3_binary.hashes import S3_SHA256_HASHES


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
            f"Hash mismatch for {s3_path}, expected {expected_hash} but got {hash_string}")


def _download_path_with_retry(*args, **kwargs):
    for i in range(5):
        try:
            return urllib.request.urlretrieve(*args, **kwargs)
        except Exception as e:
            print(f"Download failed: {e}")
            if i == 4:
                raise
            print("Retrying download...")
            time.sleep(3)
            continue


def download_s3_binary(
        s3_path: str,
        local_path: str = None,
) -> None:
    if local_path is None:
        local_path = s3_path.split("/")[-1]
    tempfile_name = None
    with tempfile.NamedTemporaryFile(delete=False) as temp_file:
        tempfile_name = temp_file.name
        _download_path_with_retry(s3_path, temp_file.name)
        _verify_s3_hash(s3_path, temp_file.name, S3_SHA256_HASHES[s3_path])
        shutil.copy(temp_file.name, local_path)
    if tempfile_name and os.path.exists(tempfile_name):
        os.unlink(tempfile_name)
