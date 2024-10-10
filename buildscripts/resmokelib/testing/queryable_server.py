#!/usr/bin/env python3
"""Implements an ephemeral queryable server."""

import argparse
import logging
import os
import tempfile
import urllib
from http.server import BaseHTTPRequestHandler, HTTPServer
from os.path import getsize, join
from urllib.parse import urlparse

import bson


class QueryableHandler(BaseHTTPRequestHandler):
    # Default dbpath.
    dbpath = "/data/db"
    ephemeral_files = {}
    verbose = False

    def _set_headers(self):
        """
        Sets the response headers.
        """
        self.send_response(200)
        self.send_header("Content-type", "application/json")
        self.end_headers()

    def _parse_url(self):
        """
        Parse the URL and return the path.

        Example:
        /os_wt_recovery_write?snapshotId=12345&filename=WiredTiger.turtle.set&offset=0&length=1470
        returns /os_wt_recovery_write
        """
        return urlparse(self.path).path

    def _parse_params(self):
        """
        Parse the URL and return the parameters in a dictionary

        Example:
        /os_wt_recovery_write?snapshotId=12345&filename=WiredTiger.turtle.set&offset=0&length=1470
        returns {filename: ["WiredTiger.turtle"], offset: ["0"], length: ["1470"]}
        """
        return urllib.parse.parse_qs(urlparse(self.path).query)

    def _set_response_bson(self, response):
        """
        Encodes the response as BSON.
        """
        if self.verbose:
            logging.info("Response: %s\n", str(response))
        self.wfile.write(bson.BSON.encode(response))

    def _set_response_raw(self, response):
        """
        Encodes the response as binary data.
        """
        if self.verbose:
            logging.info("Response: %s\n", str(response))
        self.wfile.write(response)

    def do_GET(self):
        """
        Routes GET calls.
        """
        logging.info("GET request,\nPath: %s\nHeaders:\n%s\n", str(self.path), str(self.headers))
        self._set_headers()

        match self._parse_url():
            case "/os_list":
                self._set_response_bson(self.os_list())
            case "/os_read":
                self._set_response_raw(self.os_read())
            case "/os_wt_recovery_open_file":
                self.os_wt_recovery_open_file()
            case "/os_wt_rename_file":
                self.os_wt_rename_file()
            case _:
                raise Exception("Invalid path: " + self._parse_url())

    def do_POST(self):
        """
        Routes POST calls.
        """
        logging.info("POST request,\nPath: %s\nHeaders:\n%s\n", str(self.path), str(self.headers))

        content_length = int(self.headers["Content-Length"])
        content = self.rfile.read(content_length)

        self._set_headers()
        match self._parse_url():
            case "/os_wt_recovery_write":
                self.os_wt_recovery_write(content)
            case _:
                raise Exception("Invalid path: " + self._parse_url())

    def os_list(self):
        """
        Lists persisted files and their size. Does not report ephemeral files.
        """
        dir_files = []

        for root, dirs, files in os.walk(self.dbpath):
            for file in files:
                rel_dir = os.path.relpath(root, self.dbpath)
                rel_file = os.path.join(rel_dir, file)
                if rel_file.startswith("./"):
                    rel_file = rel_file[2:]
                dir_files.append({"filename": rel_file, "fileSize": getsize(join(root, file))})

        return {"ok": True, "files": dir_files}

    def os_read(self):
        """
        Reads data at the specified offset and length.
        """
        params = self._parse_params()
        file_name = params["filename"][0]
        offset = int(params["offset"][0])
        length = int(params["length"][0])
        file_path = os.path.join(self.dbpath, file_name)

        logging.info(
            "Reading %s length %s at offset %s\n", str(file_path), str(length), str(offset)
        )

        if file_name not in self.ephemeral_files:
            self.ephemeral_files[file_name] = tempfile.TemporaryFile()
            if os.path.isfile(file_path):
                file = open(file_path, "rb")
                self.ephemeral_files[file_name].write(file.read())
                file.close()

        file = self.ephemeral_files[file_name]
        file.seek(offset)
        return file.read(length)

    def os_wt_recovery_write(self, content):
        """
        Writes data at the specified offset and length.
        """
        params = self._parse_params()
        file_name = params["filename"][0]
        offset = int(params["offset"][0])
        length = int(params["length"][0])
        assert len(content) == length

        file_path = os.path.join(self.dbpath, file_name)

        logging.info(
            "Writing %s length %s at offset %s\n", str(file_path), str(length), str(offset)
        )

        if file_name not in self.ephemeral_files:
            self.ephemeral_files[file_name] = tempfile.TemporaryFile()
            if os.path.isfile(file_path):
                file = open(file_path, "rb")
                self.ephemeral_files[file_name].write(file.read())
                file.close()

        file = self.ephemeral_files[file_name]
        file.seek(offset)
        file.write(content)

    def os_wt_recovery_open_file(self):
        """
        Creates a new file.
        """
        params = self._parse_params()
        file_name = params["filename"][0]

        if file_name in self.ephemeral_files:
            return

        logging.info("Create file %s\n", str(file_name))

        self.ephemeral_files[file_name] = tempfile.TemporaryFile()

    def os_wt_rename_file(self):
        """
        Renames the file. Assumes the file is already loaded.
        """
        params = self._parse_params()
        from_file = params["from"][0]
        to_file = params["to"][0]

        logging.info("Renaming file %s to %s\n", str(from_file), str(to_file))

        assert from_file in self.ephemeral_files
        self.ephemeral_files[to_file] = self.ephemeral_files.pop(from_file)


def run(server_class=HTTPServer, port=8080, dbpath="/data/db", verbose=False):
    logging.basicConfig(level=logging.INFO)
    server_address = ("", int(port))

    handler = QueryableHandler
    handler.dbpath = dbpath
    handler.verbose = bool(verbose)

    httpd = server_class(server_address, handler)
    logging.info("Starting queryable server...\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    httpd.server_close()
    logging.info("Stopping queryable server...\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="port to run on")
    parser.add_argument("--dbpath", help="snapshot directory")
    parser.add_argument("--verbose", help="more verbose logging")
    args = parser.parse_args()

    run(**vars(args))
