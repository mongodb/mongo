#! /usr/bin/env python3
"""Mock Config Expand Endpoint."""

import argparse
import http.server
import json
import logging
import time
import urllib.parse

connect_count = 0

class ConfigExpandRestHandler(http.server.BaseHTTPRequestHandler):
    """
    Handle requests from mongod during config expansion.
    """

    protocol_version = 'HTTP/1.1'

    def handle(self):
        global connect_count
        connect_count += 1
        super(ConfigExpandRestHandler, self).handle()

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
            response = b''
            content_type = 'text/plain'
            connection = 'keep-alive'

            if path == '/reflect/string':
                # Parses 'string' value from query string and echoes it back.
                response = ','.join(query['string']).encode()
            elif path == '/reflect/yaml':
                # Parses 'json' value from query string as JSON and reencodes as YAML.
                response = query['yaml'][0].encode()
                content_type = 'text/yaml'
            elif path == '/connect_count':
                global connect_count
                response = str(connect_count).encode()
            elif path == '/connection_close':
                connection = 'close'
                response = b'closed'
            else:
                code = http.HTTPStatus.NOT_FOUND
                response = b'Unknown URL'

            self.send_response(code)
            self.send_header('content-type', content_type)
            self.send_header('content-length', len(response))
            self.send_header('connection', connection)
            self.end_headers()
            self.wfile.write(response)

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
