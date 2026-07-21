import os
import shutil
import sys
from urllib.parse import urlparse

import boto3
import botocore.session
import requests

# Evergreen exposes these credentials into the environment (via `add_expansions_to_env`) under the
# expansion names below rather than the standard AWS_* variables that boto's default credential chain
# reads. Many mciuploads objects are private (permissions: private / visibility: signed), so without
# these an S3 GetObject is unauthenticated and returns 403. Pick them up explicitly when present.
_EVG_ACCESS_KEY_ENV = "aws_key_new"
_EVG_SECRET_KEY_ENV = "aws_secret"


def _evergreen_credentials():
    """Return (access_key, secret_key) from Evergreen expansion env vars, or None if not both set."""
    access_key = os.environ.get(_EVG_ACCESS_KEY_ENV)
    secret_key = os.environ.get(_EVG_SECRET_KEY_ENV)
    if access_key and secret_key:
        return access_key, secret_key
    return None


def get_s3_client(**client_kwargs):
    botocore.session.Session()

    # Prefer Evergreen-provided credentials so signed requests can reach private mciuploads objects.
    # Skip this for explicitly-unsigned clients (public buckets) and when the caller passes its own.
    is_unsigned = (
        getattr(client_kwargs.get("config"), "signature_version", None) == botocore.UNSIGNED
    )
    caller_supplied_creds = (
        "aws_access_key_id" in client_kwargs or "aws_secret_access_key" in client_kwargs
    )
    if not is_unsigned and not caller_supplied_creds:
        creds = _evergreen_credentials()
        if creds:
            (
                client_kwargs["aws_access_key_id"],
                client_kwargs["aws_secret_access_key"],
            ) = creds

    if sys.platform in ("win32", "cygwin"):
        # These overriden values can be found here
        # https://github.com/boto/botocore/blob/13468bc9d8923eccd0816ce2dd9cd8de5a6f6e0e/botocore/configprovider.py#L49C7-L49C7
        # This is due to the backwards breaking changed python introduced https://bugs.python.org/issue36264
        botocore_session = botocore.session.Session(
            session_vars={
                "config_file": (
                    None,
                    "AWS_CONFIG_FILE",
                    os.path.join(os.environ["HOME"], ".aws", "config"),
                    None,
                ),
                "credentials_file": (
                    None,
                    "AWS_SHARED_CREDENTIALS_FILE",
                    os.path.join(os.environ["HOME"], ".aws", "credentials"),
                    None,
                ),
            }
        )
        boto3.setup_default_session(botocore_session=botocore_session)
    return boto3.client("s3", **client_kwargs)


def extract_s3_bucket_key(url: str) -> tuple[str, str]:
    """
    Extracts the S3 bucket name and object key from an HTTP(s) S3 URL.

    Supports both:
      - https://bucket.s3.amazonaws.com/key/…
      - https://bucket.s3.<region>.amazonaws.com/key/…

    Returns:
      (bucket, key)
    """
    parsed = urlparse(url)
    # Hostname labels, e.g. ["bucket","s3","us-east-1","amazonaws","com"]
    bucket = parsed.hostname.split(".")[0]
    key = parsed.path.lstrip("/")
    return bucket, key


def download_from_s3_with_requests(url, output_file, raise_on_error=False):
    with requests.get(url, stream=True) as reader:
        if raise_on_error:
            reader.raise_for_status()
        with open(output_file, "wb") as file_handle:
            shutil.copyfileobj(reader.raw, file_handle)


class S3AccessError(RuntimeError):
    """Raised when an S3 object cannot be accessed with either signed or unsigned requests."""


def download_from_s3_with_boto(url, output_file):
    bucket_name, object_key = extract_s3_bucket_key(url)
    try:
        s3_client = get_s3_client()
        s3_client.download_file(bucket_name, object_key, output_file)
        return
    except botocore.exceptions.ClientError as e:
        error_code = e.response.get("Error", {}).get("Code", "")
        # A 403 from a signed request means our credentials lack permission. This can happen for a
        # genuinely public bucket (creds unnecessary), so retry once anonymously.
        if error_code != "403":
            raise
        try:
            s3_client = get_s3_client(
                config=botocore.client.Config(signature_version=botocore.UNSIGNED)
            )
            s3_client.download_file(bucket_name, object_key, output_file)
        except botocore.exceptions.ClientError as unsigned_err:
            unsigned_code = unsigned_err.response.get("Error", {}).get("Code", "")
            # Both signed and anonymous access were forbidden: the object is private (S3 returns 403
            # rather than 404 to callers without ListBucket). Raw S3 access will never succeed here;
            # the caller must supply a presigned URL or credentials authorized for this object.
            # Fail loudly with a diagnostic instead of surfacing a bare 403 that downstream code
            # masks into a misleading failure (e.g. a broken image build followed by DNS errors).
            if unsigned_code == "403":
                raise S3AccessError(
                    f"Access denied (403) fetching s3://{bucket_name}/{object_key} with both signed "
                    "and anonymous requests. The object is private; download it via a presigned URL "
                    "or with credentials authorized for this object rather than raw S3 access."
                ) from unsigned_err
            raise
