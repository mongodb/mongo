import os
import shutil
import sys
from urllib.parse import urlparse

import boto3
import botocore.session
import requests


def get_s3_client():
    botocore.session.Session()

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
    return boto3.client("s3")

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


def download_from_s3_with_requests(url, output_file):
    with requests.get(url, stream=True) as reader:
        with open(output_file, "wb") as file_handle:
            shutil.copyfileobj(reader.raw, file_handle)


def download_from_s3_with_boto(url, output_file):
    bucket_name, object_key = extract_s3_bucket_key(url)
    s3_client = get_s3_client()
    s3_client.download_file(bucket_name, object_key, output_file)
