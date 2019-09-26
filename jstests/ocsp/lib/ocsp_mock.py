#! /usr/bin/env python3
"""
Python script to interface as a mock OCSP responder.
"""

import argparse
import logging
from datetime import datetime
from ocspresponder import OCSPResponder, CertificateStatus

FAULT_REVOKED = "revoked"
FAULT_UNKNOWN = "unknown"

fault = None

def validate(serial: int):
    if fault == FAULT_REVOKED:
        return (CertificateStatus.revoked, datetime.today())
    elif fault == FAULT_UNKNOWN:
        return (CertificateStatus.unknown, None)
    return (CertificateStatus.good, None)

def get_cert(serial: int):
    """
    Assume the certificates are stored in the ``certs`` directory with the
    serial as base filename.
    """
    with open('jstests/libs/ocsp/serial/server-%s.pem' % serial, 'r') as f:
        return f.read().strip()

def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="MongoDB Mock OCSP Responder.")

    parser.add_argument('-p', '--port', type=int, default=8080, help="Port to listen on")

    parser.add_argument('--ca_file', type=str, required=True, help="CA file for OCSP responder")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--ocsp_responder_cert', type=str, required=True, help="OCSP Responder Certificate")

    parser.add_argument('--ocsp_responder_key', type=str, required=True, help="OCSP Responder Keyfile")

    parser.add_argument('--fault', choices=[FAULT_REVOKED, FAULT_UNKNOWN], type=str, help="Specify a specific fault to test")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    global fault
    if args.fault:
        fault = args.fault

    print('Initializing OCSP Responder')
    app = OCSPResponder(
        args.ca_file, args.ocsp_responder_cert, args.ocsp_responder_key,
        validate_func=validate,
        cert_retrieve_func=get_cert,
    )

    if args.verbose:
        app.serve(args.port, debug=True)
    else:
        app.serve(args.port)

    print('Mock OCSP Responder is running on port %s' % (str(args.port)))

if __name__ == '__main__':
    main()
