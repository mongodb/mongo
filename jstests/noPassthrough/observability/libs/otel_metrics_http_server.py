#! /usr/bin/env python3
"""Mock OTLP HTTP metrics endpoint that records request headers."""

import argparse
import http.server
import json


class OtelMetricsHttpHandler(http.server.BaseHTTPRequestHandler):
    """Accept OTLP metrics export requests and append request metadata to a file."""

    protocol_version = "HTTP/1.1"

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length:
            self.rfile.read(content_length)

        record = {
            "path": self.path,
            "headers": {key: value for key, value in self.headers.items()},
        }
        with open(self.server.output_file, "a", encoding="utf-8") as output:
            output.write(json.dumps(record) + "\n")

        self.send_response(200)
        self.send_header("Content-Type", "application/x-protobuf")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format, *args):
        return


def run(port, output_file):
    http.server.HTTPServer.protocol_version = "HTTP/1.1"
    httpd = http.server.HTTPServer(("127.0.0.1", port), OtelMetricsHttpHandler)
    httpd.output_file = output_file
    print(f"Mock OTLP Metrics HTTP Server Listening on port {port}", flush=True)
    httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="MongoDB mock OTLP metrics HTTP endpoint.")
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
