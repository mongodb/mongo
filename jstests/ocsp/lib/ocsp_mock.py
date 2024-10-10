#! /usr/bin/env python3
"""
Python script to interface as a mock OCSP responder.
"""

import argparse
import atexit
import logging
import os
import sys
import time

sys.path.append(os.path.join(os.getcwd(), "src", "third_party", "mock_ocsp_responder"))

import mock_ocsp_responder

logger = logging.getLogger(__name__)


@atexit.register
def on_exit():
    logger.debug("Mock OCSP Responder is exiting")


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="MongoDB Mock OCSP Responder.")

    parser.add_argument("-p", "--port", type=int, default=8080, help="Port to listen on")

    parser.add_argument("-b", "--bind_ip", type=str, default=None, help="IP to listen on")

    parser.add_argument("--ca_file", type=str, required=True, help="CA file for OCSP responder")

    parser.add_argument("-v", "--verbose", action="count", help="Enable verbose tracing")

    parser.add_argument(
        "--ocsp_responder_cert", type=str, required=True, help="OCSP Responder Certificate"
    )

    parser.add_argument(
        "--ocsp_responder_key", type=str, required=True, help="OCSP Responder Keyfile"
    )

    parser.add_argument(
        "--fault",
        choices=[mock_ocsp_responder.FAULT_REVOKED, mock_ocsp_responder.FAULT_UNKNOWN, None],
        default=None,
        type=str,
        help="Specify a specific fault to test",
    )

    parser.add_argument(
        "--next_update_seconds",
        type=int,
        default=32400,
        help="Specify how long the OCSP response should be valid for",
    )

    parser.add_argument(
        "--response_delay_seconds",
        type=int,
        default=0,
        help="Delays the response by this number of seconds",
    )

    parser.add_argument(
        "--include_extraneous_status",
        action="store_true",
        help="Include status of extraneous certificates in the response",
    )

    parser.add_argument(
        "--issuer_hash_algorithm",
        type=str,
        default="sha1",
        help="Algorithm to use when hashing issuer name and key",
    )

    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(module)s: %(message)s")
    logging.Formatter.converter = time.gmtime

    logger.info("Initializing OCSP Responder")
    mock_ocsp_responder.init_responder(
        issuer_cert=args.ca_file,
        responder_cert=args.ocsp_responder_cert,
        responder_key=args.ocsp_responder_key,
        fault=args.fault,
        next_update_seconds=args.next_update_seconds,
        response_delay_seconds=args.response_delay_seconds,
        include_extraneous_status=args.include_extraneous_status,
        issuer_hash_algorithm=args.issuer_hash_algorithm,
    )

    logger.debug("Mock OCSP Responder will be started on port %s" % (str(args.port)))
    mock_ocsp_responder.init(port=args.port, debug=args.verbose, host=args.bind_ip)


if __name__ == "__main__":
    main()
