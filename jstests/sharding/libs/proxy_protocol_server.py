#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.

This script is a wrapper around the [proxy-protocol][1] package.
The most recent source code is available on [GitHub][2].

The installed version is listed in the
"[tool.poetry.group.testing.dependencies]" section of `pyproject.toml`.

[1]: https://pypi.org/project/proxy-protocol/
[2]: https://github.com/icgood/proxy-protocol/
"""

import sys

from asyncio.base_events import BaseEventLoop
from proxyprotocol.server.main import main

# We want to know when the proxy protocol server is ready to accept connections; so, we log to
# standard output "Now listening on [...]" after `listen()` has been called on the actual socket. In
# order to do this, we need to modify the behavior of `proxyprotocol.server.main`. The simplest
# thing is to "monkey patch" the standard method
# [asyncio.base_events.BaseEventLoop.create_server][1] so that it logs after the server is created.
#
# [1]: https://github.com/python/cpython/blob/5c19c5bac6abf3da97d1d9b80cfa16e003897096/Lib/asyncio/base_events.py#L1429
original_create_server = BaseEventLoop.create_server


async def monkeypatched_create_server(self, protocol_factory, host, port, *args, **kwargs):
    result = await original_create_server(self, protocol_factory, host, port, *args, **kwargs)
    print(f"Now listening on {host}:{port}")
    return result


if __name__ == "__main__":
    BaseEventLoop.create_server = monkeypatched_create_server
    sys.exit(main())
