import argparse
import sys

import requests

from buildscripts.s3_binary.download import download_s3_binary


def url_exists(url, timeout=5):
    try:
        response = requests.head(url, allow_redirects=True, timeout=timeout)
        return response.status_code == 200
    except requests.RequestException:
        return False

if __name__ == "__main__":
    
    parser = argparse.ArgumentParser(description="Download and verify S3 binary.")
    parser.add_argument("s3_path", help="S3 URL to download from")
    parser.add_argument("local_path", nargs="?", help="Optional output file path")

    args = parser.parse_args()

    if url_exists(args.s3_path):
        if not download_s3_binary(args.s3_path, args.local_path, True):
            sys.exit(1)