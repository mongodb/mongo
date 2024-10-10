import argparse
import concurrent.futures
import glob
import gzip
import json
import os
import shutil
import sys
import time
from typing import Dict, List, Optional

import boto3
import requests

from buildscripts.util.read_config import read_config_file


def process_file(
    file: str,
    aws_secret: str,
    aws_key: str,
    project: str,
    variant: str,
    version_id: str,
    revision: int,
    task_name: str,
    file_number: int,
    upload_name: str,
    start_time: int,
) -> Optional[Dict[str, str]]:
    print(f"{file} started compressing at {time.time() - start_time}")
    compressed_file = f"{file}.gz"
    with open(file, "rb") as f_in:
        with gzip.open(compressed_file, "wb") as f_out:
            shutil.copyfileobj(f_in, f_out)

    print(f"{file} finished compressing at {time.time() - start_time}")

    s3_client = boto3.client("s3", aws_access_key_id=aws_key, aws_secret_access_key=aws_secret)
    basename = os.path.basename(compressed_file)
    object_path = (
        f"{project}/{variant}/{version_id}/{task_name}-{revision}-{file_number}/{basename}"
    )
    extra_args = {"ContentType": "application/gzip", "ACL": "public-read"}
    try:
        s3_client.upload_file(compressed_file, "mciuploads", object_path, ExtraArgs=extra_args)
    except Exception as ex:
        print(f"ERROR: failed to upload file to s3 {file}", file=sys.stderr)
        print(ex, file=sys.stderr)
        return None

    url = f"https://mciuploads.s3.amazonaws.com/{object_path}"
    name = f"{upload_name} {file_number} ({basename})"

    # Sanity check to ensure the url exists
    r = requests.head(url)
    if r.status_code != 200:
        print(
            f"ERROR: Could not verify that {compressed_file} was uploaded to {url}", file=sys.stderr
        )
        return None

    print(f"{compressed_file} uploaded at {time.time() - start_time} to {url}")

    # The information needed for attach.artifacts
    task_artifact = {
        "name": name,
        "link": url,
        "visibility": "public",
    }

    return task_artifact


def main(output_file: str, patterns: List[str], display_name: str, expansions_file: str) -> int:
    if not output_file.endswith(".json"):
        print("WARN: filename input should end with `.json`", file=sys.stderr)

    expansions = read_config_file(expansions_file)

    aws_access_key = expansions.get("aws_key", None)
    aws_secret_key = expansions.get("aws_secret", None)

    if not aws_access_key or not aws_secret_key:
        print("ERROR: AWS credentials not found in expansions", file=sys.stderr)
        return 1

    project = expansions.get("project")
    build_variant = expansions.get("build_variant")
    version_id = expansions.get("version_id")
    revision = expansions.get("revision")
    task_name = expansions.get("task_name")

    start_time = time.time()
    files = set()

    for pattern in patterns:
        files.update({path for path in glob.glob(pattern) if os.path.isfile(path)})

    files = list(files)

    if not files:
        print("No files found for the input, exiting early")
        return 0

    uploads = []
    cores = os.cpu_count()
    with concurrent.futures.ThreadPoolExecutor(max_workers=cores) as executor:
        futures = []
        for i, path in enumerate(files):
            file_number = i + 1
            futures.append(
                executor.submit(
                    process_file,
                    file=path,
                    aws_secret=aws_secret_key,
                    aws_key=aws_access_key,
                    project=project,
                    variant=build_variant,
                    version_id=version_id,
                    revision=revision,
                    task_name=task_name,
                    file_number=file_number,
                    upload_name=display_name,
                    start_time=start_time,
                )
            )

        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            if result:
                uploads.append(result)

    if uploads:
        with open(output_file, "w") as file:
            json.dump(uploads, file, indent=4)

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog="FastArchiver",
        description="This improves archiving times of a large amount of big files in evergreen "
        "by compressing and uploading them asynchronously. "
        "This also uses pigz, which is a multithreaded implementation of gzip, "
        "to improve gzipping times.",
    )

    parser.add_argument(
        "--output-file", "-f", help="Name of output attach.artifacts file.", required=True
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
    parser.add_argument(
        "--display-name", "-n", help="The display name of the file in evergreen", required=True
    )
    parser.add_argument(
        "--expansions-file",
        "-e",
        help="Expansions file to read task info and aws credentials from.",
        default="../expansions.yml",
    )

    args = parser.parse_args()
    exit(main(args.output_file, args.patterns, args.display_name, args.expansions_file))
