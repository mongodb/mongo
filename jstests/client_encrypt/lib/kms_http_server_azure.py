#! /usr/bin/env python3
"""Mock Azure KMS Endpoint."""
import argparse
import base64
import http
import json
import logging
import urllib.parse
import sys

import kms_http_common

SUPPORTED_FAULT_TYPES = [
    kms_http_common.FAULT_ENCRYPT,
    kms_http_common.FAULT_ENCRYPT_CORRECT_FORMAT,
    kms_http_common.FAULT_DECRYPT,
    kms_http_common.FAULT_DECRYPT_CORRECT_FORMAT,
    kms_http_common.FAULT_DECRYPT_WRONG_KEY,
    kms_http_common.FAULT_OAUTH,
    kms_http_common.FAULT_OAUTH_CORRECT_FORMAT,
]

SECRET_PREFIX = "00SECRET"
FAKE_OAUTH_TOKEN = "omg_im_an_oauth_token"

URL_PATH_OAUTH_AUDIENCE = "/token"
URL_PATH_OAUTH_SCOPE = "/auth/cloudkms"
URL_PATH_MOCK_KEY = "/keys/my_key/"


class AzureKmsHandler(kms_http_common.KmsHandlerBase):
    """
    Handle requests from Azure KMS Monitoring and test commands
    """

    def do_POST(self):
        """Serve a POST request."""
        print("Received POST: " + self.path)
        parts = urllib.parse.urlsplit(self.path)
        path = parts[2]

        if path == "/my_tentant/oauth2/v2.0/token":
            self._do_oauth_request()
        elif path.startswith(URL_PATH_MOCK_KEY):
            self._do_operation()
        else:
            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.end_headers()
            self.wfile.write("Unknown URL".encode())

    def _do_operation(self):
        clen = int(self.headers.get("content-length"))

        raw_input = self.rfile.read(clen)

        print(f"RAW INPUT: {str(raw_input)}")

        if not self.headers["Host"] == "localhost":
            data = "Unexpected host"
            self._send_reply(data.encode("utf-8"))

        if not self.headers["Authorization"] == f"Bearer {FAKE_OAUTH_TOKEN}":
            data = "Unexpected bearer token"
            self._send_reply(data.encode("utf-8"))

        parts = urllib.parse.urlsplit(self.path)
        operation = parts.path.split('/')[-1]

        if operation == "wrapkey":
            self._do_encrypt(raw_input)
        elif operation == "unwrapkey":
            self._do_decrypt(raw_input)
        else:
            self._send_reply(f"Unknown operation: {operation}".encode("utf-8"))

    def _do_encrypt(self, raw_input):
        request = json.loads(raw_input)

        print(request)

        plaintext = request["value"]

        ciphertext = SECRET_PREFIX.encode() + plaintext.encode()
        ciphertext = base64.urlsafe_b64encode(ciphertext).decode()

        if kms_http_common.fault_type and kms_http_common.fault_type.startswith(kms_http_common.FAULT_ENCRYPT) \
                and not kms_http_common.disable_faults:
            return self._do_encrypt_faults(ciphertext)

        response = {
            "value": ciphertext,
            "kid": "my_key",
        }

        self._send_reply(json.dumps(response).encode('utf-8'))

    def _do_encrypt_faults(self, raw_ciphertext):
        kms_http_common.stats.fault_calls += 1

        if kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT:
            self._send_reply("Internal Error of some sort.".encode(), http.HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_ENCRYPT_CORRECT_FORMAT:
            response = {
                "error": {
                    "code": "bad",
                    "message": "Error encrypting message",
                }
            }
            self._send_reply(json.dumps(response).encode('utf-8'))
            return

        raise ValueError("Unknown Fault Type: " + kms_http_common.fault_type)

    def _do_decrypt(self, raw_input):
        request = json.loads(raw_input)
        blob = base64.urlsafe_b64decode(request["value"]).decode()

        print("FOUND SECRET: " + blob)

        # our "encrypted" values start with the word SECRET_PREFIX otherwise they did not come from us
        if not blob.startswith(SECRET_PREFIX):
            raise ValueError()

        blob = blob[len(SECRET_PREFIX):]

        if kms_http_common.fault_type and kms_http_common.fault_type.startswith(kms_http_common.FAULT_DECRYPT) \
                and not kms_http_common.disable_faults:
            return self._do_decrypt_faults(blob)

        response = {
            "kid": "my_key",
            "value": blob,
        }

        self._send_reply(json.dumps(response).encode('utf-8'))

    def _do_decrypt_faults(self, blob):
        kms_http_common.stats.fault_calls += 1

        if kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT:
            self._send_reply("Internal Error of some sort.".encode(), http.HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT_WRONG_KEY:
            response = {
                "kid":  "my_key",
                "value": "ta7DXE7J0OiCRw03dYMJSeb8nVF5qxTmZ9zWmjuX4zW/SOorSCaY8VMTWG+cRInMx/rr/+QeVw2WjU2IpOSvMg==",
            }
            self._send_reply(json.dumps(response).encode('utf-8'))
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_DECRYPT_CORRECT_FORMAT:
            response = {
                "error": {
                    "code": "bad",
                    "message": "Error decrypting message",
                }
            }
            self._send_reply(json.dumps(response).encode('utf-8'))
            return

        raise ValueError("Unknown Fault Type: " + kms_http_common.fault_type)

    def _do_oauth_request(self):
        clen = int(self.headers.get('content-length'))

        raw_input = self.rfile.read(clen)

        print(f"RAW INPUT: {str(raw_input)}")

        if not self.headers["Host"] == "localhost":
            data = "Unexpected host"
            self._send_reply(data.encode("utf-8"))

        if kms_http_common.fault_type and kms_http_common.fault_type.startswith(kms_http_common.FAULT_OAUTH) \
                and not kms_http_common.disable_faults:
            return self._do_oauth_faults()

        response = {
            "access_token": FAKE_OAUTH_TOKEN,
            "scope": self.headers["Host"] + URL_PATH_OAUTH_SCOPE,
            "token_type": "Bearer",
            "expires_in": 3600,
        }

        self._send_reply(json.dumps(response).encode("utf-8"))

    def _do_oauth_faults(self):
        kms_http_common.stats.fault_calls += 1

        if kms_http_common.fault_type == kms_http_common.FAULT_OAUTH:
            self._send_reply("Internal Error of some sort.".encode(), http.HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        elif kms_http_common.fault_type == kms_http_common.FAULT_OAUTH_CORRECT_FORMAT:
            response = {
                "error": "Azure OAuth Error",
                "error_description": "FAULT_OAUTH_CORRECT_FORMAT",
                "error_uri": "https://mongodb.com/whoopsies.pdf",
            }
            self._send_reply(json.dumps(response).encode('utf-8'))
            return

        raise ValueError("Unknown Fault Type: " + kms_http_common.fault_type)


def main():
    """Main Method."""
    parser = argparse.ArgumentParser(description='MongoDB Mock Azure KMS Endpoint.')

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

    kms_http_common.run(args.port, args.cert_file, args.ca_file, AzureKmsHandler)


if __name__ == '__main__':
    main()
