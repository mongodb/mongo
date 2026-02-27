#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.
"""

import sys
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

# Monkey-patch the library's run() function to add close_clients() call during shutdown.
#
# IMPORTANT: The code below is copied from proxyprotocol.server.main.run() v0.11.3
# (see python3-venv/lib/python3.*/site-packages/proxyprotocol/server/main.py)
# with minimal modifications.
#
# CHANGES FROM LIBRARY SOURCE:
# 1. Added support for injecting TLV vectors into the proxy protocol header 
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

        loop.add_signal_handler(signal.SIGINT, forever.cancel)
        loop.add_signal_handler(signal.SIGTERM, forever.cancel)

        try:
            await forever
        except CancelledError:
            pass
    # --- END: Code copied from library's run() function ---
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
    print("Starting proxy protocol server...")
    sys.exit(main())
