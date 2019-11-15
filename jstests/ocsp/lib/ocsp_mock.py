#! /usr/bin/env python3
"""
Python script to interface as a mock OCSP responder.
"""

import argparse
import logging
import sys
import os

sys.path.append(os.path.join(os.getcwd() ,'src', 'third_party', 'mock_ocsp_responder'))

import mock_ocsp_responder

def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="MongoDB Mock OCSP Responder.")

    parser.add_argument('-p', '--port', type=int, default=8080, help="Port to listen on")

    parser.add_argument('--ca_file', type=str, required=True, help="CA file for OCSP responder")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--ocsp_responder_cert', type=str, required=True, help="OCSP Responder Certificate")

    parser.add_argument('--ocsp_responder_key', type=str, required=True, help="OCSP Responder Keyfile")

    parser.add_argument('--fault', choices=[mock_ocsp_responder.FAULT_REVOKED, mock_ocsp_responder.FAULT_UNKNOWN], type=str, help="Specify a specific fault to test")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    print('Initializing OCSP Responder')
    app = mock_ocsp_responder.OCSPResponder(args.ca_file, args.ocsp_responder_cert, args.ocsp_responder_key, args.fault)

    if args.verbose:
        app.serve(args.port, debug=True)
    else:
        app.serve(args.port)

    print('Mock OCSP Responder is running on port %s' % (str(args.port)))

if __name__ == '__main__':
    main()
