#! /usr/bin/env python3
"""Mock OTLP HTTP endpoint that records request paths, headers and exported spans."""

import argparse
import base64
import gzip
import http.server
import json

from google.protobuf import json_format
from opentelemetry.proto.collector.trace.v1 import trace_service_pb2

_ID_FIELDS = ("traceId", "spanId", "parentSpanId")


def _to_hex_ids(value):
    """Recursively rewrites the base64-encoded span/trace id fields of a decoded record to hex."""
    if isinstance(value, list):
        return [_to_hex_ids(element) for element in value]
    if not isinstance(value, dict):
        return value
    return {
        key: base64.b64decode(val).hex()
        if key in _ID_FIELDS and isinstance(val, str)
        else _to_hex_ids(val)
        for key, val in value.items()
    }


def decode_trace_request(body):
    """Decodes an OTLP/protobuf ExportTraceServiceRequest into its OTLP/JSON representation."""
    request = trace_service_pb2.ExportTraceServiceRequest()
    request.ParseFromString(body)
    return _to_hex_ids(json_format.MessageToDict(request))


class OtelHttpHandler(http.server.BaseHTTPRequestHandler):
    """Accept OTLP export requests and append request metadata to a file."""

    protocol_version = "HTTP/1.1"

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length else b""

        record = {
            "path": self.path,
            "headers": {key: value for key, value in self.headers.items()},
        }

        # Only traces are decoded; metrics requests are recorded for their path and headers alone.
        if body and self.path.endswith("/v1/traces"):
            if self.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
            record.update(decode_trace_request(body))

        with open(self.server.output_file, "ab", buffering=0) as output:
            output.write((json.dumps(record) + "\n").encode("utf-8"))

        self.send_response(200)
        self.send_header("Content-Type", "application/x-protobuf")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format, *args):
        return


def run(port, output_file):
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), OtelHttpHandler)
    httpd.output_file = output_file
    print(f"Mock OTLP HTTP Server Listening on port {port}", flush=True)
    httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="MongoDB mock OTLP HTTP endpoint.")
    parser.add_argument("-p", "--port", type=int, required=True, help="Port to listen on")
    parser.add_argument(
        "-o",
        "--output-file",
        required=True,
        help="File to append captured request metadata as JSON lines",
    )
    args = parser.parse_args()
    run(args.port, args.output_file)


if __name__ == "__main__":
    main()
