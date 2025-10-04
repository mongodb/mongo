import threading
from http.server import HTTPServer

from buildscripts.resmokelib.testing import queryable_server
from buildscripts.resmokelib.testing.hooks import interface


class QueryableServerHook(interface.Hook):
    DESCRIPTION = "Starts the queryable server before each test for queryable restores. Restarts the queryable server between tests."

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, queryable_dbpath=None):
        interface.Hook.__init__(self, hook_logger, fixture, QueryableServerHook.DESCRIPTION)

        assert queryable_dbpath

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
        self._queryable_server_thread.stop()


class QueryableServerThread(threading.Thread):
    def __init__(self, queryable_dbpath):
        threading.Thread.__init__(self, name="QueryableServerThread")
        self._queryable_dbpath = queryable_dbpath
        self._stop_event = threading.Event()

    def run(self):
        server_address = ("", 8080)

        handler = queryable_server.QueryableHandler
        handler.dbpath = self._queryable_dbpath
        handler.verbose = False

        httpd = HTTPServer(server_address, handler)
        httpd.timeout = 1

        while not self._stop_event.is_set():
            httpd.handle_request()

        httpd.server_close()

    def stop(self):
        self._stop_event.set()
        self.join()

        queryable_server.QueryableHandler.ephemeral_files = {}
