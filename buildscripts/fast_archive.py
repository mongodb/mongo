import argparse
import json
from multiprocessing.pool import ThreadPool
import boto3
import requests
from buildscripts.util.read_config import read_config_file
import glob
import os
import sys
import time
from typing import Dict, List, Optional

from pigz_python import PigzFile


def upload_file(file: str, aws_secret: str, aws_key: str, project: str, variant: str,
                version_id: str, revision: int, task_name: str, file_number: int, upload_name: str,
                start_time: int) -> Optional[Dict[str, str]]:
    s3_client = boto3.client('s3', aws_access_key_id=aws_key, aws_secret_access_key=aws_secret)
    basename = os.path.basename(file)
    object_path = f"{project}/{variant}/{version_id}/{task_name}-{revision}-{file_number}/{basename}"
    extra_args = {"ContentType": "application/gzip", "ACL": "public-read"}
    try:
        s3_client.upload_file(file, "mciuploads", object_path, ExtraArgs=extra_args)
    except Exception as ex:
        print(f"ERROR: failed to upload file to s3 {file}", file=sys.stderr)
        print(ex, file=sys.stderr)
        return None

    url = f"https://mciuploads.s3.amazonaws.com/{object_path}"
    name = f"{upload_name} {file_number} ({basename})"

    # Sanity check to ensure the url exists
    r = requests.head(url)
    if r.status_code != 200:
        print(f"ERROR: Could not verify that {file} was uploaded to {url}", file=sys.stderr)
        return None

    print(f"{file} uploaded at {time.time() - start_time} to {url}")

    # The information needed for attach.artifacts
    task_artifact = {
        "name": name,
        "link": url,
        "visibility": "public",
    }

    return task_artifact


def main(output_file: str, patterns: List[str], display_name: str, expansions_file: str,
         timeout: int) -> int:

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
        files.update(set(glob.glob(pattern)))

    files = list(files)

    if not files:
        print("No files found for the input, exiting early")
        return 0

    pool = ThreadPool(processes=1)
    pool_results = []
    file_count = len(files)

    for i, path in enumerate(files):
        file_number = i + 1
        print(f"Started processing file {file_number} of {file_count}: {path}")
        pigz_file = PigzFile(path)
        pigz_file.process_compression_target()
        compressed_file = f"{path}.gz"
        print(f"Finished {path} at time: {time.time() - start_time}")

        pool_results.append(
            pool.apply_async(
                upload_file,
                args=(compressed_file, aws_secret_key, aws_access_key, project, build_variant,
                      version_id, revision, task_name, file_number, display_name, start_time)))

        # Ignore the timeout if it is the last element in the array
        if time.time() - start_time > timeout and i != len(files) - 1:
            print(f"Timeout hit at: {time.time() - start_time}")

            unprocessed_files = files[i + 1:]
            print("The following files were left out do to a timeout:")
            for unprocessed_file in unprocessed_files:
                print(unprocessed_file)

            break

    pool.close()
    uploads = []

    for pool_result in pool_results:
        upload = pool_result.get()
        if upload:
            uploads.append(upload)

    if uploads:
        with open(output_file, "w") as file:
            json.dump(uploads, file, indent=4)

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog='FastArchiver',
        description='This improves archiving times of a large amount of big files in evergreen '
        'by compressing and uploading them asynchronously. '
        'This also uses pigz, which is a multithreaded implementation of gzip, '
        'to improve gzipping times.')

    parser.add_argument("--output-file", "-f", help="Name of output attach.artifacts file.",
                        required=True)
    parser.add_argument("--pattern", "-p", help="glob patterns of files to be archived.",
                        dest="patterns", action="append", default=[], required=True)
    parser.add_argument("--display-name", "-n", help="The display name of the file in evergreen",
                        required=True)
    parser.add_argument("--expansions-file", "-e",
                        help="Expansions file to read task info and aws credentials from.",
                        default="../expansions.yml")
    # 21600 = 6 hrs in seconds
    parser.add_argument(
        "--timeout", "-t", help=
        "The time (in seconds) allowed archive and upload everything up. This triggers after the current file being processed is complete.",
        type=int, default=21600)

    args = parser.parse_args()
    exit(
        main(args.output_file, args.patterns, args.display_name, args.expansions_file,
             args.timeout))
