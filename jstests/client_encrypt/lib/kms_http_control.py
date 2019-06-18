#! /usr/bin/env python3
"""
Python script to interact with mock AWS KMS HTTP server.
"""

import argparse
import json
import logging
import sys
import urllib.request
import ssl

import kms_http_common

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description='MongoDB Mock AWS KMS Endpoint.')

    parser.add_argument('-p', '--port', type=int, default=8000, help="Port to listen on")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--ca_file', type=str, required=True, help="TLS CA PEM file")

    parser.add_argument('--query', type=str, help="Query endpoint <name>")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    url_str = "https://localhost:" + str(args.port)
    if args.query == "stats":
        url_str += kms_http_common.URL_PATH_STATS
    elif args.query == "disable_faults":
        url_str += kms_http_common.URL_DISABLE_FAULTS
    elif args.query == "enable_faults":
        url_str += kms_http_common.URL_ENABLE_FAULTS
    else:
        print("Unknown query type")
        sys.exit(1)

    context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=args.ca_file)

    with urllib.request.urlopen(url_str, context=context) as f:
        print(f.read().decode('utf-8'))

    sys.exit(0)


if __name__ == '__main__':

    main()
