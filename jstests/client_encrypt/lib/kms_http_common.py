"""Common code for mock kms http endpoint."""
import http.server
import json
import ssl
import urllib.parse
from abc import abstractmethod

URL_PATH_STATS = "/stats"
URL_DISABLE_FAULTS = "/disable_faults"
URL_ENABLE_FAULTS = "/enable_faults"

"""Fault which causes encrypt to return 500."""
FAULT_ENCRYPT = "fault_encrypt"

"""Fault which causes encrypt to return an error that contains a type and message"""
FAULT_ENCRYPT_CORRECT_FORMAT = "fault_encrypt_correct_format"

"""Fault which causes encrypt to return wrong fields in JSON."""
FAULT_ENCRYPT_WRONG_FIELDS = "fault_encrypt_wrong_fields"

"""Fault which causes encrypt to return bad BASE64."""
FAULT_ENCRYPT_BAD_BASE64 = "fault_encrypt_bad_base64"

"""Fault which causes decrypt to return 500."""
FAULT_DECRYPT = "fault_decrypt"

"""Fault which causes decrypt to return an error that contains a type and message"""
FAULT_DECRYPT_CORRECT_FORMAT = "fault_decrypt_correct_format"

"""Fault which causes decrypt to return wrong key."""
FAULT_DECRYPT_WRONG_KEY = "fault_decrypt_wrong_key"

"""Fault which causes an OAuth request to return an 500."""
FAULT_OAUTH = "fault_oauth"

"""Fault which causes an OAuth request to return an error response"""
FAULT_OAUTH_CORRECT_FORMAT = "fault_oauth_correct_format"


class Stats:
    """Stats class shared between client and server."""

    def __init__(self):
        self.encrypt_calls = 0
        self.decrypt_calls = 0
        self.fault_calls = 0

    def __repr__(self):
        return json.dumps({
            'decrypts': self.decrypt_calls,
            'encrypts': self.encrypt_calls,
            'faults': self.fault_calls,
        })


class KmsHandlerBase(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        """Serve a Test GET request."""
        print("Received GET: " + self.path)
        parts = urllib.parse.urlsplit(self.path)
        path = parts[2]

        if path == URL_PATH_STATS:
            self._do_stats()
        elif path == URL_DISABLE_FAULTS:
            self._do_disable_faults()
        elif path == URL_ENABLE_FAULTS:
            self._do_enable_faults()
        else:
            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.end_headers()
            self.wfile.write("Unknown URL".encode())

    @abstractmethod
    def do_POST(self):
        """Serve a POST request."""
        pass

    def _send_reply(self, data, status=http.HTTPStatus.OK):
        print("Sending Response: " + data.decode())

        self.send_response(status)
        self.send_header("content-type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()

        self.wfile.write(data)

    @abstractmethod
    def _do_encrypt(self, raw_input):
        pass

    @abstractmethod
    def _do_encrypt_faults(self, raw_ciphertext):
        pass

    @abstractmethod
    def _do_decrypt(self, raw_input):
        pass

    @abstractmethod
    def _do_decrypt_faults(self, blob):
        pass

    def _send_header(self):
        self.send_response(http.HTTPStatus.OK)
        self.send_header("content-type", "application/octet-stream")
        self.end_headers()

    def _do_stats(self):
        self._send_header()

        self.wfile.write(str(stats).encode('utf-8'))

    def _do_disable_faults(self):
        global disable_faults
        disable_faults = True
        self._send_header()

    def _do_enable_faults(self):
        global disable_faults
        disable_faults = False
        self._send_header()


def run(port, cert_file, ca_file, handler_class, server_class=http.server.HTTPServer):
    """Run web server."""
    server_address = ('', port)

    httpd = server_class(server_address, handler_class)

    httpd.socket = ssl.wrap_socket(httpd.socket,
                                   certfile=cert_file,
                                   ca_certs=ca_file, server_side=True)

    print(f"Mock KMS Web Server Listening on {str(server_address)}")

    httpd.serve_forever()


# Pass this data out of band instead of storing it in AwsKmsHandler since the
# BaseHTTPRequestHandler does not call the methods as object methods but as class methods. This
# means there is not self.
stats = Stats()
disable_faults = False
fault_type = None
