import argparse
import os
import sys
from urllib.parse import urlparse

import boto3
import botocore.config
import botocore.exceptions

from buildscripts.s3_binary.download import download_s3_binary

# mciuploads binaries are private ("signed" visibility), so an unauthenticated HEAD always
# comes back 403 regardless of whether the object exists. Check existence with a signed
# request instead. Even signed, a missing object comes back as 403 rather than 404 when the
# caller lacks s3:ListBucket (same handling as fetch_optional_s3_targz.py).
_NOT_PRESENT_CODES = frozenset({"404", "NoSuchKey", "NoSuchBucket", "403", "AccessDenied"})


def url_exists(url: str, timeout: int = 5) -> bool:
    parsed = urlparse(url)
    bucket = parsed.netloc.split(".")[0]
    key = parsed.path.lstrip("/")

    client = boto3.client(
        "s3",
        aws_access_key_id=os.environ.get("aws_key_new") or os.environ.get("AWS_ACCESS_KEY_ID"),
        aws_secret_access_key=os.environ.get("aws_secret")
        or os.environ.get("AWS_SECRET_ACCESS_KEY"),
        config=botocore.config.Config(connect_timeout=timeout, read_timeout=timeout),
    )
    try:
        client.head_object(Bucket=bucket, Key=key)
        return True
    except botocore.exceptions.ClientError as err:
        code = err.response.get("Error", {}).get("Code", "") or str(
            err.response.get("ResponseMetadata", {}).get("HTTPStatusCode", "")
        )
        if code in _NOT_PRESENT_CODES:
            return False
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Download and verify S3 binary.")
    parser.add_argument("s3_path", help="S3 URL to download from")
    parser.add_argument("local_path", nargs="?", help="Optional output file path")

    args = parser.parse_args()

    if not url_exists(args.s3_path):
        print(f"jstestshell not present at {args.s3_path}; skipping fetch")
    else:
        print(f"jstestshell found at {args.s3_path}; downloading")
        if not download_s3_binary(args.s3_path, args.local_path, True):
            sys.exit(1)
        if args.local_path and os.path.exists(args.local_path):
            size = os.path.getsize(args.local_path)
            print(f"jstestshell downloaded and sha256 verified: {args.local_path} ({size} bytes)")
        else:
            print("jstestshell downloaded and sha256 verified")
