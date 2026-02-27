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
from typing import Dict, Optional, Tuple, List

import proxyprotocol.server.main
from proxyprotocol.server.main import main

# Optional TLVs to append to every emitted PROXY protocol v2 header.
_tlv_structs = None  # type: Optional[Dict[int, bytes]]
_ssl_tlv_structs = None  # type: Optional[Dict[int, bytes]]
_tlv_file_path = None  # type: Optional[str]


# See setTLVs docstring in jstests/sharding/libs/proxy_protocol.js for format.
def _parse_pp2_tlv_structs_json(raw_json_tlv: str) -> Dict[int, bytes]:
    import json

    def is_valid_type(type_num: int) -> bool:
        return (
            (0x01 <= type_num <= 0x05)
            or (0x20 <= type_num <= 0x25)
            or (type_num == 0x30)
            or (0xE0 <= type_num <= 0xEF)
        )

    def parse_one_struct(obj: dict) -> Tuple[int, bytes]:
        if "type" not in obj:
            raise ValueError("TLV object missing required field 'type'")
        type_num = obj["type"]
        if type(type_num) is not int:
            raise ValueError("TLV type expected to be a number")
        if not is_valid_type(type_num):
            raise ValueError("TLV type is invalid")
        if "value" not in obj:
            raise ValueError("TLV object missing required field 'value'")
        return type_num, str(obj["value"]).encode("utf-8")

    def parse_ssl_tlv(obj: dict) -> Dict[int, bytes]:
        ret: Dict[int, bytes] = {}
        if "ssl" in obj:
            ssl_obj = obj["ssl"]
            if not isinstance(ssl_obj, list):
                raise ValueError(f"Invalid TLV entry (expected list): {obj!r}")
            for sub_entry in ssl_obj:
                type_num, val = parse_one_struct(sub_entry)
                ret[type_num] = val
        return ret

    def parse_common(entry: dict, tlvs, ssl_tlvs):
        maybe_ssl_obj = parse_ssl_tlv(entry)
        if maybe_ssl_obj:
            if ssl_tlvs:
                raise ValueError(f"Expected one ssl entry but received multiple")
            ssl_tlvs.update(maybe_ssl_obj)
            return

        type_num, val = parse_one_struct(entry)
        tlvs[type_num] = val

    tlvs: Dict[int, bytes] = {}
    ssl_tlvs: Dict[int, bytes] = {}
    parsed = json.loads(raw_json_tlv)
    if isinstance(parsed, dict):
        parse_common(parsed, tlvs, ssl_tlvs)
    elif isinstance(parsed, list):
        for entry in parsed:
            if not isinstance(entry, dict):
                raise ValueError(f"Invalid TLV entry (expected object): {entry!r}")
            parse_common(entry, tlvs, ssl_tlvs)
    else:
        raise ValueError("TLV JSON must be an object or array of objects")

    return tlvs, ssl_tlvs


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
# 3. Added support for injecting TLV vectors into the proxy protocol header
#
_original_run = proxyprotocol.server.main.run


async def _patched_run(args):
    import asyncio
    import signal
    from asyncio import CancelledError
    from contextlib import AsyncExitStack
    from functools import partial

    from proxyprotocol.build import build_transport_result
    from proxyprotocol.dnsbl import Dnsbl
    from proxyprotocol.server import Address
    from proxyprotocol.server.protocol import DownstreamProtocol, UpstreamProtocol
    from proxyprotocol.result import is_ipv4, is_ipv6, is_unix
    from proxyprotocol.result import (
        ProxyResult,
        ProxyResultIPv4,
        ProxyResultIPv6,
        ProxyResultUnix,
    )
    from proxyprotocol.tlv import ProxyProtocolTLV, ProxyProtocolSSLTLV

    # --- BEGIN: Code copied from library's run() function ---
    loop = asyncio.get_running_loop()

    services = [(Address(source, server=True), Address(dest)) for (source, dest) in args.services]
    buf_len = args.buf_len
    dnsbl = Dnsbl.load(args.dnsbl, timeout=args.dnsbl_timeout)

    # --- BEGIN MODIFICATION: Injecting TLV into proxy protocol header ---
    def _reload_tlv_file() -> None:
        global _tlv_structs, _ssl_tlv_structs
        path = _tlv_file_path
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw = f.read()
                if raw.strip():
                    while "\n" in raw:
                        line, raw = raw.split("\n", 1)
                        if raw:
                            # Only read the last line
                            continue
                        _tlv_structs, _ssl_tlv_structs = _parse_pp2_tlv_structs_json(line)
                else:
                    _tlv_structs, _ssl_tlv_structs = {}, {}
        except FileNotFoundError:
            pass
        except Exception as e:
            _tlv_structs, _ssl_tlv_structs = {}, {}
            print(f"Failed to reload TLV file {path!r}: {e}", file=sys.stderr, flush=True)

    def _with_tlv(result: ProxyResult, tlv: ProxyProtocolTLV) -> ProxyResult:
        if is_ipv4(result):
            return ProxyResultIPv4(result.source, result.dest, protocol=result.protocol, tlv=tlv)
        if is_ipv6(result):
            return ProxyResultIPv6(result.source, result.dest, protocol=result.protocol, tlv=tlv)
        if is_unix(result):
            return ProxyResultUnix(result.source, result.dest, protocol=result.protocol, tlv=tlv)
        return result

    class UpstreamProtocolWithTLV(UpstreamProtocol):
        def build_pp_header(self, dnsbl, result):
            _reload_tlv_file()
            if self.downstream.connected:
                result = build_transport_result(
                    self.downstream.transport,
                    dnsbl=dnsbl,
                )
            cur_tlv = _tlv_structs or {}
            cur_ssl_tlv = _ssl_tlv_structs or {}

            if not cur_tlv and not cur_ssl_tlv:
                return self.pp.pack(result)
            return self.pp.pack(
                _with_tlv(
                    result,
                    ProxyProtocolTLV(init=cur_tlv, ssl=ProxyProtocolSSLTLV(init=cur_ssl_tlv)),
                )
            )

    new_server = partial(DownstreamProtocol, UpstreamProtocolWithTLV, loop, buf_len, dnsbl)
    # --- END MODIFICATION ---

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


def _parse_pp2_tlvs_from_argv(argv: List[str]) -> Tuple[List[str], Optional[str]]:
    """
    Strip wrapper-specific TLV flags from argv.

    Supported flags:
      - --pp2-tlv-file PATH
    """
    filtered: List[str] = []
    tlv_file: Optional[str] = None

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--pp2-tlv-file":
            if i + 1 >= len(argv):
                raise ValueError("--pp2-tlv-file requires a path argument")
            tlv_file = argv[i + 1]
            i += 2
            continue
        filtered.append(arg)
        i += 1

    return filtered, tlv_file


if __name__ == "__main__":
    # Parse and strip wrapper-specific flags before invoking the library's CLI.
    try:
        filtered_argv, tlv_file = _parse_pp2_tlvs_from_argv(sys.argv[1:])
        sys.argv = [sys.argv[0], *filtered_argv]
        if tlv_file:
            _tlv_file_path = tlv_file
    except Exception as e:
        print(f"Failed to parse TLV flags: {e}", file=sys.stderr, flush=True)
        sys.exit(2)
    sys.exit(main())
