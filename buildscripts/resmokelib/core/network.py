"""
Helper to reserve a network port.
"""

from __future__ import absolute_import

import socket as _socket
import threading


class UnusedPort(object):
    """
    Acquires an unused port from the OS.
    """

    # Use a set to keep track of ports that are acquired from the OS to avoid returning duplicates.
    # We do not remove ports from this set because they are used throughout the lifetime of
    # resmoke.py for started mongod/mongos processes.
    _ALLOCATED_PORTS = set()
    _ALLOCATED_PORTS_LOCK = threading.Lock()

    def __init__(self):
        self.num = None
        self.__socket = None

    def __enter__(self):
        while True:
            socket = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
            socket.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
            socket.bind(("0.0.0.0", 0))

            port = socket.getsockname()[1]
            with UnusedPort._ALLOCATED_PORTS_LOCK:
                # Check whether the OS has already given us 'port'.
                if port in UnusedPort._ALLOCATED_PORTS:
                    socket.close()
                    continue

                UnusedPort._ALLOCATED_PORTS.add(port)

            self.num = port
            self.__socket = socket
            return self

    def __exit__(self, *exc_info):
        if self.__socket is not None:
            self.__socket.close()
