"""
Helper to reserve a network port.
"""

from __future__ import absolute_import

import socket


class UnusedPort(object):
    """
    Acquires a unused port.
    """

    def __init__(self):
        self.num = None

    def __enter__(self):
        self.__socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.__socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.__socket.bind(("0.0.0.0", 0))

        addr, port = self.__socket.getsockname()
        self.num = port

        return self

    def __exit__(self, *exc_info):
        self.__socket.close()
