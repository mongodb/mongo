#! /usr/bin/env python3
"""Mock Config Expand Endpoint."""

import argparse
import http.server
import json
import logging
import time
import urllib.parse
import yaml

class ConfigExpandRestHandler(http.server.BaseHTTPRequestHandler):
    """
    Handle requests from mongod during config expansion.
    """

    def do_GET(self):
        """Serve a Test GET request."""
        parts = urllib.parse.urlsplit(self.path)
        path = parts.path
        query = urllib.parse.parse_qs(parts.query)

        code = int(query.get('code', [http.HTTPStatus.OK])[0])
        sleep = int(query.get('sleep', [0])[0])
        if sleep > 0:
            time.sleep(sleep)

        try:
            if path == '/reflect/string':
                # Parses 'string' value from query string and echoes it back.
                self.send_response(code)
                self.send_header('content-type', 'text/plain')
                self.end_headers()
                self.wfile.write(','.join(query['string']).encode())
                return

            if path == '/reflect/yaml':
                # Parses 'json' value from query string as JSON and reencodes as YAML.
                self.send_response(code)
                self.send_header('content-type', 'text/yaml')
                self.end_headers()
                self.wfile.write(yaml.dump(json.loads(query['json'][0]), default_flow_style=False).encode())
                return

            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.send_header('content-type', 'text/plain')
            self.end_headers()
            self.wfile.write('Unknown URL'.encode())

        except BrokenPipeError as err:
            # Broken pipe is reasonale if we're deliberately going slow.
            if sleep == 0:
                raise err

    def do_POST(self):
        self.send_response(http.HTTPStatus.NOT_FOUND)
        self.send_header('content-type', 'text/plain')
        self.end_headers()
        self.wfile.write('POST not supported')

def run(port):
    """Run web server."""

    http.server.HTTPServer.protocol_version = "HTTP/1.1"
    server_address = ('', port)
    httpd = http.server.HTTPServer(server_address, ConfigExpandRestHandler)

    print("Mock Web Server Listening on %s" % (str(server_address)))
    httpd.serve_forever()

def main():
    """Main Method."""

    parser = argparse.ArgumentParser(description='MongoDB Mock Config Expandsion REST Endpoint.')

    parser.add_argument('-p', '--port', type=int, default=8000, help="Port to listen on")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    run(args.port)


if __name__ == '__main__':

    main()
