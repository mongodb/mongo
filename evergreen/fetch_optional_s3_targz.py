#!/usr/bin/env python3
"""Fetch an optional .tar.gz artifact from S3 and extract it.

This replaces the ``s3.get`` Evergreen command for artifacts that are only
produced on some build variants (e.g. the Message Filter Plugin). The stock
``s3.get`` command always retries 5 times with backoff (~25s) before honoring
``optional: true``. On the (common) case where the object legitimately does not
exist, that stall is paid by every non-plugin variant.

This script distinguishes "the object is not there" from a transient failure:

  * A missing object -- surfaced by S3 as either ``404 NoSuchKey`` or, when the
    caller lacks ``s3:ListBucket``, ``403 AccessDenied`` -- is treated as "not
    present". We exit 0 immediately with no retries.
  * A transient failure (5xx, throttling, connection reset, timeout) is retried
    with bounded exponential backoff.

Credentials are read from the environment (populated by
``add_expansions_to_env: true`` in the Evergreen func), falling back to standard
AWS_* variables.
"""

import argparse
import os
import sys
import tarfile
import tempfile
import time

import boto3
import botocore.exceptions
from botocore.config import Config

# S3 error codes that mean "the object is not there for us" -- never retried.
_NOT_PRESENT_CODES = frozenset({"404", "NoSuchKey", "NoSuchBucket", "403", "AccessDenied"})

# Error codes that are worth retrying.
_TRANSIENT_CODES = frozenset(
    {
        "500",
        "503",
        "InternalError",
        "ServiceUnavailable",
        "SlowDown",
        "Throttling",
        "ThrottlingException",
        "RequestTimeout",
        "RequestTimeTooSkewed",
    }
)


def _log(msg: str) -> None:
    print(f"[fetch_optional_s3_targz] {msg}", flush=True)


def _make_client(aws_key: str | None, aws_secret: str | None):
    config = Config(retries={"max_attempts": 0}, connect_timeout=15, read_timeout=60)
    kwargs = {"config": config}
    if aws_key and aws_secret:
        kwargs["aws_access_key_id"] = aws_key
        kwargs["aws_secret_access_key"] = aws_secret
    return boto3.client("s3", **kwargs)


def _error_code(err: botocore.exceptions.ClientError) -> str:
    code = err.response.get("Error", {}).get("Code", "")
    status = str(err.response.get("ResponseMetadata", {}).get("HTTPStatusCode", ""))
    return code or status


def download(
    client, bucket: str, key: str, dest: str, max_attempts: int, base_backoff: float, required: bool
) -> bool:
    """Download s3://bucket/key to dest.

    Returns True on success, False if the object is not present (only when
    ``required`` is False). Raises on exhausted transient retries, or on a
    missing object when ``required`` is True.
    """
    for attempt in range(1, max_attempts + 1):
        try:
            _log(f"downloading s3://{bucket}/{key} (attempt {attempt}/{max_attempts})")
            client.download_file(bucket, key, dest)
            return True
        except botocore.exceptions.ClientError as err:
            code = _error_code(err)
            if code in _NOT_PRESENT_CODES:
                if required:
                    _log(f"object not present (code={code}) but download is required; failing")
                    raise
                _log(f"object not present (code={code}); treating as optional, skipping")
                return False
            if code in _TRANSIENT_CODES and attempt < max_attempts:
                backoff = base_backoff * (2 ** (attempt - 1))
                _log(f"transient error (code={code}); retrying in {backoff:.1f}s")
                time.sleep(backoff)
                continue
            raise
        except (
            botocore.exceptions.ConnectionError,
            botocore.exceptions.ReadTimeoutError,
            botocore.exceptions.EndpointConnectionError,
        ) as err:
            if attempt < max_attempts:
                backoff = base_backoff * (2 ** (attempt - 1))
                _log(f"connection error ({type(err).__name__}); retrying in {backoff:.1f}s")
                time.sleep(backoff)
                continue
            raise
    return False


def extract(tarball: str, extract_to: str) -> None:
    os.makedirs(extract_to, exist_ok=True)
    _log(f"extracting {tarball} -> {extract_to}")
    with tarfile.open(tarball, "r:gz") as tar:
        tar.extractall(path=extract_to)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--remote-file", required=True, help="S3 object key")
    parser.add_argument("--extract-to", required=True, help="local dir to extract into")
    parser.add_argument(
        "--aws-key", default=os.environ.get("aws_key_new") or os.environ.get("AWS_ACCESS_KEY_ID")
    )
    parser.add_argument(
        "--aws-secret",
        default=os.environ.get("aws_secret") or os.environ.get("AWS_SECRET_ACCESS_KEY"),
    )
    parser.add_argument("--max-attempts", type=int, default=4)
    parser.add_argument("--base-backoff", type=float, default=1.0)
    parser.add_argument(
        "--skip-unless-set",
        metavar="EXPANSION_NAME",
        help=(
            "Name of an env var/expansion (e.g. mfp_compile_variant) that gates this fetch. "
            "If unset or empty, skip everything and exit 0. If set, the object is required: "
            "a missing object is an error, not a skip."
        ),
    )
    args = parser.parse_args()

    if args.skip_unless_set is not None:
        if not os.environ.get(args.skip_unless_set):
            _log(f"{args.skip_unless_set} not set; nothing to fetch, skipping")
            return 0
        _log(f"{args.skip_unless_set} set; download is required")
    required = args.skip_unless_set is not None

    client = _make_client(args.aws_key, args.aws_secret)

    with tempfile.NamedTemporaryFile(suffix=".tgz", delete=False) as tmp:
        tmp_path = tmp.name
    try:
        present = download(
            client,
            args.bucket,
            args.remote_file,
            tmp_path,
            args.max_attempts,
            args.base_backoff,
            required,
        )
        if not present:
            return 0
        extract(tmp_path, args.extract_to)
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
    _log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
