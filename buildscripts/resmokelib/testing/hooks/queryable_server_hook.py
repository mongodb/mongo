import os
import socketserver
import threading
from http.server import HTTPServer

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing import queryable_server
from buildscripts.resmokelib.testing.hooks import interface


class QueryableServerHook(interface.Hook):
    DESCRIPTION = "Starts the queryable server before each test for queryable restores. Restarts the queryable server between tests."

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, queryable_dbpath=None):
        interface.Hook.__init__(self, hook_logger, fixture, QueryableServerHook.DESCRIPTION)

        assert queryable_dbpath

        if not os.path.isabs(queryable_dbpath):
            base = _config.DBPATH_PREFIX or _config.DEFAULT_DBPATH_PREFIX
            queryable_dbpath = os.path.join(base, queryable_dbpath)

        self._queryable_dbpath = queryable_dbpath
        self._stop_event = None
        self._queryable_server_thread = None

    def before_suite(self, test_report):
        return

    def after_suite(self, test_report, teardown_flag=None):
        return

    def before_test(self, test, test_report):
        self.logger.info("Starting queryable server")
        self._queryable_server_thread = QueryableServerThread(self._queryable_dbpath)
        self._queryable_server_thread.start()

    def after_test(self, test, test_report):
        self.logger.info("Stopping queryable server")
        if self._queryable_server_thread is not None:
            self._queryable_server_thread.stop()


class QueryableHTTPServer(HTTPServer):
    """HTTP server that binds without a reverse DNS lookup."""

    def server_bind(self):
        # HTTPServer.server_bind() calls socket.getfqdn(), which can block for a long time and
        # delays listen() until after the test client has given up.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "localhost"
        self.server_port = self.server_address[1]


class QueryableServerThread(threading.Thread):
    def __init__(self, queryable_dbpath):
        threading.Thread.__init__(self, name="QueryableServerThread")
        self._queryable_dbpath = queryable_dbpath
        self._stop_event = threading.Event()

        handler = queryable_server.QueryableHandler
        handler.dbpath = self._queryable_dbpath
        handler.verbose = False

        # Bind in the constructor so a bind failure fails the test instead of silently killing
        # the server thread.
        self._httpd = QueryableHTTPServer(("127.0.0.1", 8080), handler)
        self._httpd.timeout = 1

    def run(self):
        while not self._stop_event.is_set():
            self._httpd.handle_request()

        self._httpd.server_close()

    def stop(self):
        self._stop_event.set()
        self.join()

        queryable_server.QueryableHandler.ephemeral_files = {}
