#!/usr/bin/env python3
"""TLS 1.3 KeyUpdate client used by jstests/ssl/tls13_windows_keyupdate.js.

mongod (Schannel) as a TLS *server* never receives a NewSessionTicket — those flow
server->client — so the server-side post-handshake path (AcceptSecurityContext) is not
exercised by the KMIP tests (which only cover mongod-as-client). A TLS 1.3 *KeyUpdate*
is the one post-handshake message a client can send to a server, so this script connects
to mongod and triggers one.

Python's ssl module does not expose SSL_key_update(), so we drive the system `openssl
s_client` CLI, whose interactive `K` command sends a TLS 1.3 key_update with
update_requested=1 (forcing the server to respond with its own KeyUpdate, exercising the
ASC output path). After the KeyUpdate we send a line of application data so the server has
to decrypt with the rotated keys — a successful decrypt proves the keys were not corrupted.

Exit codes:
  0  - the KeyUpdate sequence was sent (inspect the mongod log for the result)
  2  - openssl CLI is unavailable or too old (TLS 1.3 / KeyUpdate unsupported); test should skip
"""

import argparse
import re
import shutil
import subprocess
import sys
import time


def openssl_supports_tls13(openssl: str) -> bool:
    try:
        out = subprocess.run(
            [openssl, "version"], capture_output=True, text=True, timeout=15, check=False
        ).stdout
    except Exception as e:
        print(f"failed to run 'openssl version': {e}")
        return False
    print(f"openssl version: {out.strip()}")
    # TLS 1.3 and the s_client 'K' KeyUpdate command require OpenSSL >= 1.1.1.
    m = re.search(r"OpenSSL\s+(\d+)\.(\d+)\.(\d+)", out)
    if not m:
        # LibreSSL or unknown: assume unsupported to be safe.
        return False
    major, minor, patch = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return (major, minor, patch) >= (1, 1, 1)


def main() -> int:
    parser = argparse.ArgumentParser(description="TLS 1.3 KeyUpdate client for mongod")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument(
        "--cert",
        default=None,
        help="Combined client cert+key PEM to present (optional).",
    )
    args = parser.parse_args()

    openssl = shutil.which("openssl")
    if not openssl or not openssl_supports_tls13(openssl):
        print("OPENSSL_UNAVAILABLE: cannot send a TLS 1.3 KeyUpdate")
        return 2

    cmd = [
        openssl,
        "s_client",
        "-connect",
        f"{args.host}:{args.port}",
        "-tls1_3",
        # We do not validate mongod's certificate here; s_client continues after a verify
        # warning, and the handshake itself is all we need to reach the post-handshake phase.
    ]
    if args.cert:
        # The mongo test PEMs bundle the certificate and private key in one file.
        cmd += ["-cert", args.cert, "-key", args.cert]
    print("launching:", " ".join(cmd))
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    try:
        # Let the TLS 1.3 handshake complete before issuing commands.
        time.sleep(3)
        # 'K' (uppercase) => send key_update with update_requested=1, so mongod must reply
        # with its own KeyUpdate (exercises processPostHandshakeToken's output path).
        proc.stdin.write(b"K\n")
        proc.stdin.flush()
        time.sleep(2)
        # Application data after the KeyUpdate. mongod must decrypt this with the rotated
        # traffic keys; a successful decrypt (no SEC_E_DECRYPT_FAILURE) proves correctness.
        # The bytes themselves are not a valid wire message — mongod will reject them after
        # decrypting, which is fine; we only care that decryption succeeded.
        proc.stdin.write(b"ping-after-keyupdate\n")
        proc.stdin.flush()
        time.sleep(2)
        proc.stdin.write(b"Q\n")
        proc.stdin.flush()
    except Exception as e:
        print(f"client I/O error (continuing to drain output): {e}")

    try:
        out, _ = proc.communicate(timeout=15)
        sys.stdout.write(out.decode(errors="replace"))
    except Exception:
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=5)
            sys.stdout.write(out.decode(errors="replace"))
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
