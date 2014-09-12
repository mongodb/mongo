
"""
Very basic network helpers to allow programs to easily reserve network ports and manage timeouts.
"""

import time
import socket


class Timer(object):

    def __init__(self):
        self.start_time_secs = time.time()

    def elapsed_secs(self):
        return time.time() - self.start_time_secs


class UnusedPort(object):

    def __init__(self, port=0):
        self.unused_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.unused_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.unused_socket.bind(("0.0.0.0", port))
        self.addr, self.port = self.unused_socket.getsockname()

    def release(self):
        self.unused_socket.close()
        self.unused_socket, self.addr, self.port = None, None, None
