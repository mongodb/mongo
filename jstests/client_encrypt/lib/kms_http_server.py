#! /usr/bin/env python3
"""Mock AWS KMS Endpoint."""

import argparse
import base64
import http.server
import json
import logging
import sys
import urllib.parse

from botocore.auth import SigV4Auth
from botocore.awsrequest import AWSRequest
from botocore.credentials import Credentials

import kms_http_common

SECRET_PREFIX = "00SECRET"

# List of supported fault types
SUPPORTED_FAULT_TYPES = [
    kms_http_common.FAULT_ENCRYPT,
    kms_http_common.FAULT_ENCRYPT_CORRECT_FORMAT,
    kms_http_common.FAULT_ENCRYPT_WRONG_FIELDS,
    kms_http_common.FAULT_ENCRYPT_BAD_BASE64,
    kms_http_common.FAULT_DECRYPT,
    kms_http_common.FAULT_DECRYPT_CORRECT_FORMAT,
    kms_http_common.FAULT_DECRYPT_WRONG_KEY,
]

def get_dict_subset(headers, subset):
    ret = {}
    for header in headers.keys():
        if header.lower() in subset.lower():
            ret[header] = headers[header]
    return ret

class AwsKmsHandler(kms_http_common.KmsHandlerBase):
    """
    Handle requests from AWS KMS Monitoring and test commands
    """

    def do_POST(self):
        print("Received POST: " + self.path)
        parts = urllib.parse.urlsplit(self.path)
        path = parts[2]

        if path == "/":
            self._do_post()
        else:
            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.end_headers()
            self.wfile.write("Unknown URL".encode())

    def _do_post(self):
        clen = int(self.headers.get('content-length'))

        raw_input = self.rfile.read(clen)

        print("RAW INPUT: " + str(raw_input))

        if not self.headers["Host"] == "localhost":
            data = "Unexpected host"
            self._send_reply(data.encode("utf-8"))

        if not self._validate_signature(self.headers, raw_input):
            data = "Bad Signature"
            self._send_reply(data.encode("utf-8"))

        # X-Amz-Target: TrentService.Encrypt
        aws_operation = self.headers['X-Amz-Target']

        if aws_operation == "TrentService.Encrypt":
            kms_http_common.stats.encrypt_calls += 1
            self._do_encrypt(raw_input)
        elif aws_operation == "TrentService.Decrypt":
            kms_http_common.stats.decrypt_calls += 1
            self._do_decrypt(raw_input)
        else:
            data = "Unknown AWS Operation"
            self._send_reply(data.encode("utf-8"))

    def _validate_signature(self, headers, raw_input):
        auth_header = headers["Authorization"]
        signed_headers_start = auth_header.find("SignedHeaders")
        signed_headers = auth_header[signed_headers_start:auth_header.find(",", signed_headers_start)]
        signed_headers_dict = get_dict_subset(headers, signed_headers)

        request = AWSRequest(method="POST", url="/", data=raw_input, headers=signed_headers_dict)
        # SigV4Auth assumes this header exists even though it is not required by the algorithm
        request.context['timestamp'] = headers['X-Amz-Date']

        region_start = auth_header.find("Credential=access/") + len("Credential=access/YYYYMMDD/")
        region = auth_header[region_start:auth_header.find("/", region_start)]

        credentials = Credentials("access", "secret")
        auth = SigV4Auth(credentials, "kms", region)
        string_to_sign = auth.string_to_sign(request, auth.canonical_request(request))
        expected_signature = auth.signature(string_to_sign, request)

        signature_headers_start = auth_header.find("Signature=") + len("Signature=")
        actual_signature = auth_header[signature_headers_start:]

        return expected_signature == actual_signature

    def _do_encrypt(self, raw_input):
        request = json.loads(raw_input)

        print(request)

        plaintext = request["Plaintext"]
        keyid = request["KeyId"]

        ciphertext = SECRET_PREFIX.encode() + plaintext.encode()
        ciphertext = base64.b64encode(ciphertext).decode()

        if kms_http_common.fault_type and kms_http_common.fault_type.startswith(kms_http_common.FAULT_ENCRYPT) \
                and not kms_http_common.disable_faults:
            return self._do_encrypt_faults(ciphertext)

        response = {
            "CiphertextBlob" : ciphertext,
            "KeyId" : keyid,
        }

        self._send_reply(json.dumps(response).encode('utf-8'))

    def _do_encrypt_faults(self, raw_ciphertext):
        kms_http_common.stats.fault_calls += 1

        if kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT:
            self._send_reply("Internal Error of some sort.".encode(), http.HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT_WRONG_FIELDS:
            response = {
                "SomeBlob" : raw_ciphertext,
                "KeyId" : "foo",
            }

            self._send_reply(json.dumps(response).encode('utf-8'))
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT_BAD_BASE64:
            response = {
                "CiphertextBlob" : "foo",
                "KeyId" : "foo",
            }

            self._send_reply(json.dumps(response).encode('utf-8'))
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT_CORRECT_FORMAT:
            response = {
                "__type" : "NotFoundException",
                "Message" : "Error encrypting message",
            }

            self._send_reply(json.dumps(response).encode('utf-8'))
            return

        raise ValueError("Unknown Fault Type: " + kms_http_common.fault_type)

    def _do_decrypt(self, raw_input):
        request = json.loads(raw_input)
        blob = base64.b64decode(request["CiphertextBlob"]).decode()

        print("FOUND SECRET: " + blob)

        # our "encrypted" values start with the word SECRET_PREFIX otherwise they did not come from us
        if not blob.startswith(SECRET_PREFIX):
            raise ValueError()

        blob = blob[len(SECRET_PREFIX):]

        if kms_http_common.fault_type and kms_http_common.fault_type.startswith(kms_http_common.FAULT_DECRYPT) \
                and not kms_http_common.disable_faults:
            return self._do_decrypt_faults(blob)

        response = {
            "Plaintext" : blob,
            "KeyId" : "Not a clue",
        }

        self._send_reply(json.dumps(response).encode('utf-8'))

    def _do_decrypt_faults(self, blob):
        kms_http_common.stats.fault_calls += 1

        if kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT:
            self._send_reply("Internal Error of some sort.".encode(), http.HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT_WRONG_KEY:
            response = {
                "Plaintext" : "ta7DXE7J0OiCRw03dYMJSeb8nVF5qxTmZ9zWmjuX4zW/SOorSCaY8VMTWG+cRInMx/rr/+QeVw2WjU2IpOSvMg==",
                "KeyId" : "Not a clue",
            }

            self._send_reply(json.dumps(response).encode('utf-8'))
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT_CORRECT_FORMAT:
            response = {
                "__type" : "NotFoundException",
                "Message" : "Error decrypting message",
            }

            self._send_reply(json.dumps(response).encode('utf-8'))
            return

        raise ValueError("Unknown Fault Type: " + kms_http_common.fault_type)

def main():
    """Main Method."""
    parser = argparse.ArgumentParser(description='MongoDB Mock AWS KMS Endpoint.')

    parser.add_argument('-p', '--port', type=int, default=8000, help="Port to listen on")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--fault', type=str, help="Type of fault to inject")

    parser.add_argument('--disable-faults', action='store_true', help="Disable faults on startup")

    parser.add_argument('--ca_file', type=str, required=True, help="TLS CA PEM file")

    parser.add_argument('--cert_file', type=str, required=True, help="TLS Server PEM file")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    if args.fault:
        if args.fault not in SUPPORTED_FAULT_TYPES:
            print("Unsupported fault type %s, supports types are %s" % (args.fault, SUPPORTED_FAULT_TYPES))
            sys.exit(1)

        kms_http_common.fault_type = args.fault

    if args.disable_faults:
        kms_http_common.disable_faults = True

    kms_http_common.run(args.port, args.cert_file, args.ca_file, AwsKmsHandler)


if __name__ == '__main__':

    main()
