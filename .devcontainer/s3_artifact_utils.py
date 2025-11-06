#!/usr/bin/env python3
"""
Shared S3 artifact utilities for DevContainer setup scripts.

This module provides common functionality for downloading and verifying
artifacts from S3, used by toolchain.py and evergreen_cli.py.
"""

import hashlib
import sys
import xml.etree.ElementTree as ET
from datetime import datetime
from urllib import request
from urllib.error import HTTPError, URLError


def list_s3_objects(bucket: str, prefix: str, path_style: bool = False) -> list[dict]:
    """Query S3 REST API for objects matching prefix.

    Args:
        bucket: S3 bucket name
        prefix: Prefix to filter objects
        path_style: If True, use path-style URL (s3.amazonaws.com/bucket).
                    If False, use virtual-hosted style (bucket.s3.amazonaws.com).
                    Path-style is required for buckets with dots in the name.
    """
    try:
        if path_style:
            url = f"https://s3.amazonaws.com/{bucket}?list-type=2&prefix={prefix}"
        else:
            url = f"https://{bucket}.s3.amazonaws.com?list-type=2&prefix={prefix}"
        print(f"Querying S3: {url}", file=sys.stderr)

        with request.urlopen(url) as response:
            xml_data = response.read()

        root = ET.fromstring(xml_data)
        ns = {"s3": "http://s3.amazonaws.com/doc/2006-03-01/"}

        objects = []
        for content in root.findall("s3:Contents", ns):
            key_elem = content.find("s3:Key", ns)
            modified_elem = content.find("s3:LastModified", ns)

            if key_elem is not None and modified_elem is not None:
                objects.append(
                    {
                        "Key": key_elem.text,
                        "LastModified": datetime.fromisoformat(
                            modified_elem.text.replace("Z", "+00:00")
                        ),
                    }
                )

        return objects

    except (HTTPError, URLError, ET.ParseError) as e:
        print(f"Error querying S3: {e}", file=sys.stderr)
        sys.exit(1)


def download_file(url: str, output_path: str) -> None:
    """Download file from URL."""
    print(f"Downloading {url}...", file=sys.stderr)
    try:
        request.urlretrieve(url, output_path)
        print(f"Saved to {output_path}", file=sys.stderr)
    except (HTTPError, URLError) as e:
        print(f"Download failed: {e}", file=sys.stderr)
        sys.exit(1)


def calculate_sha256(file_path: str) -> str:
    """Calculate SHA256 checksum of file."""
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256_hash.update(chunk)
    return sha256_hash.hexdigest()
