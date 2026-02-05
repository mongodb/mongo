#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.

This script is a wrapper around the [proxy-protocol][1] package.
The most recent source code is available on [GitHub][2].

The installed version is listed in the
"[tool.poetry.group.testing.dependencies]" section of `pyproject.toml`.

This wrapper adds two patches:
1. Logs "Now listening on [...]" when the server is ready
2. Calls server.close_clients() during shutdown for Python 3.13 compatibility

[1]: https://pypi.org/project/proxy-protocol/
[2]: https://github.com/icgood/proxy-protocol/
"""

import sys
from asyncio.base_events import BaseEventLoop

import proxyprotocol.server.main
from proxyprotocol.server.main import main

# We want to know when the proxy protocol server is ready to accept connections; so, we log to
# standard output "Now listening on [...]" after `listen()` has been called on the actual socket. In
# order to do this, we need to modify the behavior of `proxyprotocol.server.main`. The simplest
# thing is to "monkey patch" the standard method
# [asyncio.base_events.BaseEventLoop.create_server][1] so that it logs after the server is created.
#
# Additionally, we store a reference to the server objects so they can be properly shut down.
#
# [1]: https://github.com/python/cpython/blob/5c19c5bac6abf3da97d1d9b80cfa16e003897096/Lib/asyncio/base_events.py#L1429
_servers = []
_original_create_server = BaseEventLoop.create_server


async def _patched_create_server(self, protocol_factory, host, port, *args, **kwargs):
    server = await _original_create_server(self, protocol_factory, host, port, *args, **kwargs)
    _servers.append(server)
    print(f"Now listening on {host}:{port}", flush=True)
    return server


BaseEventLoop.create_server = _patched_create_server

# Monkey-patch the library's run() function to add close_clients() call during shutdown.
#
# IMPORTANT: The code below is copied from proxyprotocol.server.main.run() v0.11.3
# (see python3-venv/lib/python3.*/site-packages/proxyprotocol/server/main.py)
# with minimal modifications for Python 3.13 compatibility.
#
# CHANGES FROM LIBRARY SOURCE:
# 1. Added server.close_clients() call in handle_signal() before forever.cancel()
#    - Python 3.13 added Server.close_clients() to forcibly close active connections
#    - Python 3.12+ fixed wait_closed() to correctly wait for connections
#    - Library's original code relied on Python 3.10/3.11 bug where wait_closed()
#      returned immediately even with active connections
# 2. Added logging to track signal handling and shutdown progress
#
_original_run = proxyprotocol.server.main.run


async def _patched_run(args):
    import asyncio
    import signal
    from asyncio import CancelledError
    from contextlib import AsyncExitStack
    from functools import partial

    from proxyprotocol.dnsbl import Dnsbl
    from proxyprotocol.server import Address
    from proxyprotocol.server.protocol import DownstreamProtocol, UpstreamProtocol

    # --- BEGIN: Code copied from library's run() function ---
    loop = asyncio.get_running_loop()

    services = [(Address(source, server=True), Address(dest)) for (source, dest) in args.services]
    buf_len = args.buf_len
    dnsbl = Dnsbl.load(args.dnsbl, timeout=args.dnsbl_timeout)
    new_server = partial(DownstreamProtocol, UpstreamProtocol, loop, buf_len, dnsbl)

    servers = [
        await loop.create_server(
            partial(new_server, dest), source.host, source.port or 0, ssl=source.ssl
        )
        for source, dest in services
    ]

    async with AsyncExitStack() as stack:
        for server in servers:
            await stack.enter_async_context(server)
        forever = asyncio.gather(*[server.serve_forever() for server in servers])

        # --- BEGIN MODIFICATION: Added proper shutdown sequence ---
        def handle_signal():
            print("Proxy server received shutdown signal", flush=True)

            # Step 1: Stop accepting new connections
            try:
                for server in servers:
                    server.close()
                print("Proxy server: stopped accepting new connections", flush=True)
            except Exception as e:
                print(f"Proxy server: error in close(): {e}", flush=True)

            # Step 2: Close existing client connections gracefully
            try:
                for server in servers:
                    server.close_clients()
                print("Proxy server: initiated graceful client close", flush=True)
            except Exception as e:
                print(f"Proxy server: error in close_clients(): {e}", flush=True)

            # Step 3: Schedule abort as a safety fallback after 1 second
            def abort_remaining():
                print("Proxy server: aborting any remaining connections", flush=True)
                try:
                    for server in servers:
                        server.abort_clients()
                except Exception as e:
                    print(f"Proxy server: error in abort_clients(): {e}", flush=True)

            loop.call_later(1.0, abort_remaining)

            # CHANGE: Use proper shutdown sequence per Python 3.13 docs:
            # 1. close() to stop accepting new connections
            # 2. close_clients() to gracefully close existing connections
            # 3. Schedule abort_clients() as fallback after timeout
            forever.cancel()

        # --- END MODIFICATION ---

        loop.add_signal_handler(signal.SIGINT, handle_signal)
        loop.add_signal_handler(signal.SIGTERM, handle_signal)

        try:
            await forever
        except CancelledError:
            pass
    # --- END: Code copied from library's run() function ---

    print("Proxy server shutdown complete", flush=True)
    return 0


proxyprotocol.server.main.run = _patched_run

if __name__ == "__main__":
    sys.exit(main())
