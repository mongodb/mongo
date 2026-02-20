#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.
"""

import sys
from typing import Dict, Optional, Tuple, List

import proxyprotocol.server.main
from proxyprotocol.server.main import main

# Optional TLVs to append to every emitted PROXY protocol v2 header.
_added_tlv_structs = None  # type: Optional[Dict[int, bytes]]
_tlv_file_path = None  # type: Optional[str]
_tlv_file_mtime = None  # type: Optional[int]


# Expects to be in this format:
# [{type: 0xE0, value: "val"}, {...}]
def _parse_pp2_tlv_structs_json(raw_json_tlv: str) -> Dict[int, bytes]:
    import json

    def is_valid_type(type_num: int) -> bool:
        return (
            (0x01 <= type_num <= 0x05)
            or (0x20 <= type_num <= 0x25)
            or (type_num == 0x30)
            or (0xE0 <= type_num <= 0xEF)
        )

    def parse_type(type_val) -> int:
        if not isinstance(type_val, str):
            raise ValueError(f"TLV 'type' must be a hex string, got: {type_val!r}")
        s = type_val.strip()
        # Enforce "hex" rather than decimal.
        is_hex = s.lower().startswith("0x")
        if not is_hex:
            raise ValueError(f"TLV 'type' must be hex (e.g. '0xE0'), got: {type_val!r}")
        type_num = int(s, 16)
        if not is_valid_type(type_num):
            raise ValueError(f"TLV type out of range: {type_num}")
        return type_num

    def parse_one_struct(obj: dict) -> Tuple[int, bytes]:
        if "type" not in obj:
            raise ValueError("TLV object missing required field 'type'")
        type_num = parse_type(obj["type"])
        if "value" not in obj:
            raise ValueError("TLV object missing required field 'value'")
        return type_num, str(obj["value"]).encode("utf-8")

    tlvs: Dict[int, bytes] = {}
    parsed = json.loads(raw_json_tlv)
    if isinstance(parsed, dict):
        type_num, val = parse_one_struct(parsed)
        tlvs[type_num] = val
    elif isinstance(parsed, list):
        for entry in parsed:
            if not isinstance(entry, dict):
                raise ValueError(f"Invalid TLV entry (expected object): {entry!r}")
            type_num, val = parse_one_struct(entry)
            tlvs[type_num] = val
    else:
        raise ValueError("TLV JSON must be an object or array of objects")
    return tlvs

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
    from proxyprotocol.tlv import ProxyProtocolTLV

    # --- BEGIN: Code copied from library's run() function ---
    loop = asyncio.get_running_loop()

    services = [(Address(source, server=True), Address(dest)) for (source, dest) in args.services]
    buf_len = args.buf_len
    dnsbl = Dnsbl.load(args.dnsbl, timeout=args.dnsbl_timeout)
    
    # --- BEGIN MODIFICATION: Injecting TLV into proxy protocol header ---
    def _maybe_reload_tlv_file() -> None:
        global _added_tlv_structs, _tlv_file_mtime
        path = _tlv_file_path
        if not path:
            return
        try:
            import os

            mtime = os.stat(path).st_mtime
            if _tlv_file_mtime is not None and mtime == _tlv_file_mtime:
                return
            with open(path, "r", encoding="utf-8") as f:
                raw = f.read()
            _added_tlv_structs = _parse_pp2_tlv_structs_json(raw) if raw.strip() else {}
            _tlv_file_mtime = mtime
        except FileNotFoundError:
            # The TLV file may be briefly missing during an update (i.e. tests removing then
            # re-creating it). Keep the last-known-good TLVs in that case, and force a reload
            # next time by clearing our recorded mtime.
            _tlv_file_mtime = None
        except Exception as e:
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
            _maybe_reload_tlv_file()
            if self.downstream.connected:
                result = build_transport_result(
                    self.downstream.transport,
                    unique_id=self.downstream.id,
                    dnsbl=dnsbl,
                )
            cur_added = _added_tlv_structs or {}
            if not cur_added:
                return self.pp.pack(result)
            return self.pp.pack(_with_tlv(result, ProxyProtocolTLV(init=cur_added)))

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
