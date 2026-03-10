#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.
"""

import ssl
import sys
from typing import Dict, Optional, Tuple, List

import proxyprotocol.server.main
from proxyprotocol.server.main import main

# Optional TLVs to append to every emitted PROXY protocol v2 header.
_tlv_structs = None  # type: Optional[Dict[int, bytes]]
_ssl_tlv_structs = None  # type: Optional[Dict[int, bytes]]
_tlv_file_path = None  # type: Optional[str]

# Optional Unix domain socket path for egress connections.
_unix_egress_path = None  # type: Optional[str]

# SNI store: maps id(ssl_object) -> server_name captured via sni_callback.
_sni_store = {}  # type: Dict[int, str]

# OID for the MongoDB roles X509v3 extension.
_MONGO_ROLES_OID_DOTTED = "1.3.6.1.4.1.34601.2.1.1"


def _extract_cert_info(der_cert_bytes: bytes) -> Tuple[Optional[str], Optional[bytes]]:
    """Extract subject DN (RFC 4514 string) and roles (raw DER) from a DER certificate.

    Returns (dn_string, roles_der). Either may be None if not present.
    """
    from cryptography import x509
    from cryptography.x509.oid import ObjectIdentifier

    cert = x509.load_der_x509_certificate(der_cert_bytes)

    # Subject DN as RFC 4514 string (e.g. "CN=foo,OU=bar,O=baz")
    dn_str = cert.subject.rfc4514_string()

    # Roles extension (OID 1.3.6.1.4.1.34601.2.1.1) — raw DER value
    roles_der = None
    try:
        ext = cert.extensions.get_extension_for_oid(
            ObjectIdentifier(_MONGO_ROLES_OID_DOTTED)
        )
        roles_der = ext.value.value  # raw DER bytes of the extension value
    except x509.ExtensionNotFound:
        pass

    return dn_str, roles_der


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

    services = [
        (Address(source, server=True), Address(dest))
        for (source, dest) in args.services
    ]
    buf_len = args.buf_len
    dnsbl = Dnsbl.load(args.dnsbl, timeout=args.dnsbl_timeout)

    # --- BEGIN MODIFICATION: SNI callback for capturing TLS server_name ---
    def _sni_callback(ssl_obj, server_name, ssl_ctx):
        """Capture the client's SNI (Server Name Indication) during the TLS handshake."""
        _sni_store[id(ssl_obj)] = server_name

    for source, _dest in services:
        if source.ssl is not None:
            source.ssl.sni_callback = _sni_callback
            # Python 3.13's create_default_context(CLIENT_AUTH) sets VERIFY_X509_STRICT,
            # which rejects CA certs lacking a keyUsage extension. Our test CA certs
            # don't include keyUsage, so clear this flag to stay compatible.
            source.ssl.verify_flags &= ~ssl.VERIFY_X509_STRICT
    # --- END MODIFICATION ---

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
                        _tlv_structs, _ssl_tlv_structs = _parse_pp2_tlv_structs_json(
                            line
                        )
                else:
                    _tlv_structs, _ssl_tlv_structs = {}, {}
        except FileNotFoundError:
            pass
        except Exception as e:
            _tlv_structs, _ssl_tlv_structs = {}, {}
            print(
                f"Failed to reload TLV file {path!r}: {e}", file=sys.stderr, flush=True
            )

    def _with_tlv(result: ProxyResult, tlv: ProxyProtocolTLV) -> ProxyResult:
        if is_ipv4(result):
            return ProxyResultIPv4(
                result.source, result.dest, protocol=result.protocol, tlv=tlv
            )
        if is_ipv6(result):
            return ProxyResultIPv6(
                result.source, result.dest, protocol=result.protocol, tlv=tlv
            )
        if is_unix(result):
            return ProxyResultUnix(
                result.source, result.dest, protocol=result.protocol, tlv=tlv
            )
        return result

    class UpstreamProtocolWithTLV(UpstreamProtocol):
        def build_pp_header(self, dnsbl, result):
            _reload_tlv_file()
            if self.downstream.connected:
                result = build_transport_result(
                    self.downstream.transport,
                    dnsbl=dnsbl,
                )

            # Start with file-based TLVs (backward compat with setTLVs/proxy_server_tlv_headers).
            cur_tlv = dict(_tlv_structs) if _tlv_structs else {}
            cur_ssl_tlv = dict(_ssl_tlv_structs) if _ssl_tlv_structs else {}

            # When TLS is active on the ingress, extract cert-based TLVs automatically.
            if self.downstream.connected:
                ssl_obj = self.downstream.transport.get_extra_info("ssl_object")
                if ssl_obj is not None:
                    # SNI → TLV 0x02 (AUTHORITY)
                    sni = _sni_store.pop(id(ssl_obj), None)
                    if sni and 0x02 not in cur_tlv:
                        cur_tlv[0x02] = sni.encode("utf-8")

                    # Peer certificate → subject DN (0xE0) and roles (0xE1)
                    der_cert = ssl_obj.getpeercert(binary_form=True)
                    if der_cert:
                        try:
                            dn_str, roles_der = _extract_cert_info(der_cert)
                            if dn_str and 0xE0 not in cur_ssl_tlv:
                                cur_ssl_tlv[0xE0] = dn_str.encode("utf-8")
                            if roles_der and 0xE1 not in cur_ssl_tlv:
                                cur_ssl_tlv[0xE1] = roles_der
                        except Exception as e:
                            print(
                                f"[proxy] Failed to extract cert info: {e!r}",
                                file=sys.stderr,
                                flush=True,
                            )

            if not cur_tlv and not cur_ssl_tlv:
                return self.pp.pack(result)
            return self.pp.pack(
                _with_tlv(
                    result,
                    ProxyProtocolTLV(
                        init=cur_tlv, ssl=ProxyProtocolSSLTLV(init=cur_ssl_tlv)
                    ),
                )
            )

    # --- END MODIFICATION ---

    # --- BEGIN MODIFICATION: Unix domain socket egress support ---
    # When --unix-egress is specified, override DownstreamProtocol.connection_made
    # to use loop.create_unix_connection() instead of loop.create_connection().
    downstream_cls = DownstreamProtocol
    if _unix_egress_path:

        class DownstreamProtocolUnixEgress(DownstreamProtocol):
            """Connects to a Unix domain socket instead of TCP for egress."""

            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)

            def connection_made(self, transport):
                try:
                    # Call grandparent (_Base) connection_made for basic transport setup.
                    super(DownstreamProtocol, self).connection_made(transport)

                    loop = self.loop
                    self._dnsbl_task = loop.create_task(
                        self.dnsbl.lookup(self.sock_info)
                    )
                    self._connect_task = connect_task = loop.create_task(
                        loop.create_unix_connection(
                            self._upstream_factory, _unix_egress_path
                        )
                    )
                    result = build_transport_result(transport, unique_id=self.id)
                    connect_task.add_done_callback(
                        partial(self._unix_set_client, result)
                    )
                except Exception as e:
                    print(
                        f"[proxy-uds] Error in connection_made: {e!r}",
                        file=sys.stderr,
                        flush=True,
                    )
                    import traceback

                    traceback.print_exc(file=sys.stderr)
                    if hasattr(self, "_transport") and self._transport:
                        self._transport.close()

            def _unix_set_client(self, result, connect_task):
                """Wrapper around _set_client that prints errors to stderr."""
                try:
                    _, upstream = connect_task.result()
                except Exception as e:
                    print(
                        f"[proxy-uds] Unix egress connection FAILED: {e!r}",
                        file=sys.stderr,
                        flush=True,
                    )
                    import traceback

                    traceback.print_exc(file=sys.stderr)
                # Delegate to the original _set_client for the actual handling.
                self._set_client(result, connect_task)

        downstream_cls = DownstreamProtocolUnixEgress
    # --- END MODIFICATION ---

    new_server = partial(downstream_cls, UpstreamProtocolWithTLV, loop, buf_len, dnsbl)

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


def _parse_wrapper_flags_from_argv(
    argv: List[str],
) -> Tuple[List[str], Optional[str], Optional[str]]:
    """
    Strip wrapper-specific flags from argv.

    Supported flags:
      - --pp2-tlv-file PATH
      - --unix-egress PATH   (connect egress to a Unix domain socket instead of TCP)
    """
    filtered: List[str] = []
    tlv_file: Optional[str] = None
    unix_egress: Optional[str] = None

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--pp2-tlv-file":
            if i + 1 >= len(argv):
                raise ValueError("--pp2-tlv-file requires a path argument")
            tlv_file = argv[i + 1]
            i += 2
            continue
        if arg == "--unix-egress":
            if i + 1 >= len(argv):
                raise ValueError("--unix-egress requires a path argument")
            unix_egress = argv[i + 1]
            i += 2
            continue
        filtered.append(arg)
        i += 1

    return filtered, tlv_file, unix_egress


if __name__ == "__main__":
    # Parse and strip wrapper-specific flags before invoking the library's CLI.
    try:
        filtered_argv, tlv_file, unix_egress = _parse_wrapper_flags_from_argv(
            sys.argv[1:]
        )
        sys.argv = [sys.argv[0], *filtered_argv]
        if tlv_file:
            _tlv_file_path = tlv_file
        if unix_egress:
            _unix_egress_path = unix_egress
    except Exception as e:
        print(f"Failed to parse wrapper flags: {e}", file=sys.stderr, flush=True)
        sys.exit(2)
    print("Starting proxy protocol server...")
    sys.exit(main())
