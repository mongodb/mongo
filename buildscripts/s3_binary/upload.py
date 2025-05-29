#!/usr/bin/env python3

import hashlib
import os
import pathlib
import re
import subprocess
from pprint import pformat

import typer

from buildscripts.s3_binary.hashes import S3_SHA256_HASHES


def _sha256_file(file: pathlib.Path) -> str:
    sha256_hash = hashlib.sha256()
    with open(file, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


def _upload(local_source_directory: str, s3_destination_directory: str) -> None:
    files_to_upload = []
    for file in pathlib.Path(local_source_directory).iterdir():
        files_to_upload.append(file)
    print("Please authenticate with an account that can upload to the s3 bucket mdb-build-public")
    subprocess.check_call(["aws", "configure", "sso", "--profile", "devprod-build"])

    s3_destination_directory = s3_destination_directory.rstrip("/") + "/"

    for file in files_to_upload:
        s3_path_to_check = s3_destination_directory + file.name
        print(f"Checking that {file} does not exist as {s3_path_to_check}...")
        result = subprocess.run(
            [
                "aws",
                "s3",
                "ls",
                "--profile=devprod-build",
                s3_path_to_check,
            ],
            check=False,
        )
        if result.returncode == 0:
            raise FileExistsError(
                f"{s3_path_to_check} already exists, aborting upload. Delete the file from S3 or use a different directory."
            )

    print("Storing hashes in buildscripts/s3_binary/hashes.py...")
    for file in files_to_upload:
        https_path = (
            re.sub(r"s3://(.*?)/(.*)", r"https://\1.s3.amazonaws.com/\2", s3_destination_directory)
            + file.name
        )
        S3_SHA256_HASHES[https_path] = _sha256_file(file)

    with open("buildscripts/s3_binary/hashes.py", "w", encoding="utf-8") as hash_file:
        hash_dict = (
            pformat(S3_SHA256_HASHES, indent=4).replace("'", '"').replace("}", "").replace("{", "")
        )
        hash_file.write(f"S3_SHA256_HASHES = {{\n {hash_dict}\n}}\n")

    print(f"Uploading to {s3_destination_directory}...")
    result = subprocess.check_call(
        [
            "aws",
            "s3",
            "cp",
            "--recursive",
            "--profile=devprod-build",
            local_source_directory,
            s3_destination_directory,
        ]
    )
    return False


def main(local_source_directory: str, s3_destination_directory: str) -> None:
    """Upload tool binaries to s3 and store the hash of each for secure use."""

    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
    _upload(
        local_source_directory,
        s3_destination_directory,
    )


if __name__ == "__main__":
    typer.run(main)
