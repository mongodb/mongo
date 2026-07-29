#!/usr/bin/env python3
"""A TLS server that staples a malformed OCSP response during the handshake.

Instead of serving OCSP responses over HTTP, the server accepts TLS connections and staples an undecodable OCSP
response (a single 0x00 byte) into each handshake. A TLS peer that requests OCSP stapling
(mongod always does) will then run those bytes through d2i_OCSP_RESPONSE(), which returns
nullptr. Historically the peer dereferenced that nullptr and crashed.

Stapling is performed with pyOpenSSL because the standard library ssl module does not expose
a way to set the server's stapled OCSP response. pyOpenSSL is a declared project dependency.
"""

import argparse
import logging
import os
import signal
import socket

from OpenSSL import SSL

logger = logging.getLogger(__name__)

# A single 0x00 byte is not a valid DER-encoded OCSP response, so d2i_OCSP_RESPONSE()
# returns nullptr for it. This matches the payload stapled by the repro's C server.
MALFORMED_OCSP_STAPLE = b"\x00"


def run(port, tls_cert_key_file, bind_ip):
    """Run the malformed-staple TLS server until interrupted."""
    ctx = SSL.Context(SSL.TLS_SERVER_METHOD)
    ctx.use_certificate_chain_file(tls_cert_key_file)
    ctx.use_privatekey_file(tls_cert_key_file)

    def staple_callback(_connection, _data):
        logger.info("Stapling malformed OCSP response to handshake")
        return MALFORMED_OCSP_STAPLE

    ctx.set_ocsp_server_callback(staple_callback, None)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_ip, port))
    listener.listen()

    # Printed (and flushed) so the launching jstest can detect readiness.
    print("Malformed staple TLS server listening on %s:%d" % (bind_ip, port), flush=True)

    while True:
        try:
            client_sock, peer = listener.accept()
        except OSError:
            break

        logger.info("Incoming TLS connection from %s", peer)
        tls_conn = SSL.Connection(ctx, client_sock)
        tls_conn.set_accept_state()
        try:
            # The malformed staple is delivered during the handshake, which is all that is
            # needed to drive the peer's stapled-response handling.
            tls_conn.do_handshake()
        except Exception as exc:
            # The peer is expected to abort the connection after rejecting the malformed
            # staple, so handshake failures here are normal.
            logger.info("TLS handshake ended (expected): %s", exc)
        finally:
            try:
                tls_conn.shutdown()
            except Exception:
                pass
            tls_conn.close()


def main():
    parser = argparse.ArgumentParser(description="Malformed OCSP staple TLS server.")
    parser.add_argument("-p", "--port", type=int, default=8100, help="Port to listen on")
    parser.add_argument("-b", "--bind_ip", type=str, default="0.0.0.0", help="IP to listen on")
    parser.add_argument(
        "--tls_cert_key_file",
        type=str,
        required=True,
        help="PEM file with the certificate chain and private key the TLS server presents",
    )
    parser.add_argument("-v", "--verbose", action="count", help="Enable verbose tracing")
    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(module)s: %(message)s")

    # Exit promptly and cleanly when the launching jstest sends SIGINT.
    signal.signal(signal.SIGINT, lambda _sig, _frame: os._exit(0))

    run(port=args.port, tls_cert_key_file=args.tls_cert_key_file, bind_ip=args.bind_ip)


if __name__ == "__main__":
    main()
