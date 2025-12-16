#!/usr/bin/env python3
"""
S3-based lock mechanism.

This script attempts to create a lock file in S3. If the file already exists,
it exits with failure (exit code 1). If the file doesn't exist and is successfully
created, it exits with success (exit code 0).

Usage:
    python s3_lock.py --bucket BUCKET_NAME --key LOCK_KEY [--content CONTENT]

Example:
    python s3_lock.py --bucket my-bucket --key locks/mylock.txt --content "locked by build 123"
"""

import sys

import typer
from botocore.exceptions import ClientError

from buildscripts.util.download_utils import get_s3_client


def acquire_s3_lock(bucket, key, content="locked"):
    """
    Attempt to acquire a lock by uploading a file to S3 only if it doesn't exist.

    Args:
        bucket: S3 bucket name
        key: S3 object key (file path)
        content: Content to write to the lock file

    Returns:
        True if lock was acquired (file didn't exist), False if lock already exists
    """
    s3_client = get_s3_client()

    try:
        # Try to upload the file only if it doesn't already exist
        # using the IfNoneMatch condition with "*" (no existing ETag should match)
        s3_client.put_object(
            Bucket=bucket,
            Key=key,
            Body=content.encode("utf-8"),
            IfNoneMatch="*",  # Only succeed if object doesn't exist
        )
        return True
    except ClientError as e:
        error_code = e.response.get("Error", {}).get("Code", "")

        # PreconditionFailed means the object already exists
        if error_code == "PreconditionFailed":
            return False

        # Any other error should be raised
        raise


app = typer.Typer()


@app.command()
def main(
    bucket: str = typer.Option(..., help="S3 bucket name"),
    key: str = typer.Option(..., help="S3 object key (path) for the lock file"),
    content: str = typer.Option("locked", help="Content to write to the lock file"),
):
    """Acquire an S3-based lock by creating a file if it doesn't exist."""
    try:
        lock_acquired = acquire_s3_lock(bucket, key, content)

        if lock_acquired:
            print(f"Lock acquired: s3://{bucket}/{key}")
            sys.exit(0)
        else:
            print(f"Lock already exists: s3://{bucket}/{key}", file=sys.stderr)
            sys.exit(1)

    except ClientError as e:
        print(f"S3 error: {e}", file=sys.stderr)
        sys.exit(2)
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    app()
