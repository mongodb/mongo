#! /usr/bin/env python3
"""A forged intra-cluster peer for testing the internal-auth SASL mechanism allowlist.

Speaks just enough of the MongoDB wire protocol to answer the initial handshake. Every "hello"/
"isMaster" reply advertises only the PLAIN mechanism in saslSupportedMechs, simulating a malicious
peer attempting to downgrade an egress internal-auth connection to PLAIN (which would leak the raw
keyfile in cleartext -- see SERVER-130264).

The names of all commands received are appended to --outfile, one per line, so the test can assert
that the connecting node never issues a "saslStart" against this peer.
"""

import argparse
import datetime
import socket
import struct
import threading

import bson

OP_REPLY = 1
OP_QUERY = 2004
OP_MSG = 2013

# Header is 4 int32s: messageLength, requestID, responseTo, opCode.
HEADER_FMT = "<iiii"
HEADER_LEN = struct.calcsize(HEADER_FMT)


def recv_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_message(sock):
    header = recv_exactly(sock, HEADER_LEN)
    if header is None:
        return None
    msg_len, request_id, _response_to, op_code = struct.unpack(HEADER_FMT, header)
    body = recv_exactly(sock, msg_len - HEADER_LEN)
    if body is None:
        return None
    return request_id, op_code, body


def parse_command(op_code, body):
    """Return the request BSON document, regardless of OP_QUERY vs OP_MSG framing."""
    if op_code == OP_MSG:
        # flagBits(uint32), then sections. We only handle the kind-0 body section.
        offset = 4
        kind = body[offset]
        offset += 1
        assert kind == 0, f"unexpected OP_MSG section kind {kind}"
        doc_len = struct.unpack_from("<i", body, offset)[0]
        return bson.decode(body[offset : offset + doc_len])
    if op_code == OP_QUERY:
        # flags(int32), fullCollectionName(cstring), skip(int32), return(int32), query(BSON).
        offset = 4
        end = body.index(b"\x00", offset)
        offset = end + 1 + 8  # cstring terminator + skip + return
        doc_len = struct.unpack_from("<i", body, offset)[0]
        return bson.decode(body[offset : offset + doc_len])
    raise AssertionError(f"unexpected opcode {op_code}")


def command_name(cmd):
    return next(iter(cmd.keys())) if cmd else ""


def build_hello_reply(my_host, set_name, max_wire_version):
    return {
        "isWritablePrimary": False,
        "ismaster": False,
        "secondary": True,
        "setName": set_name,
        "me": my_host,
        "hosts": [my_host],
        "maxBsonObjectSize": 16 * 1024 * 1024,
        "maxMessageSizeBytes": 48 * 1000 * 1000,
        "maxWriteBatchSize": 100000,
        "localTime": datetime.datetime.now(),
        # Reflect the connecting node's own wire version so it does not reject us as an
        # incompatible-version peer before the authentication step is reached.
        "maxWireVersion": max_wire_version,
        "minWireVersion": 0,
        # The forged part: advertise only PLAIN.
        "saslSupportedMechs": ["PLAIN"],
        "ok": 1.0,
    }


def frame_reply(request_id, op_code, reply_doc):
    encoded = bson.encode(reply_doc)
    if op_code == OP_MSG:
        payload = struct.pack("<I", 0) + b"\x00" + encoded  # flagBits + kind-0 section
        reply_op = OP_MSG
    else:
        # OP_REPLY: responseFlags(int32), cursorId(int64), startingFrom(int32), numberReturned(int32).
        payload = struct.pack("<iqii", 0, 0, 0, 1) + encoded
        reply_op = OP_REPLY
    total_len = HEADER_LEN + len(payload)
    header = struct.pack(HEADER_FMT, total_len, 0, request_id, reply_op)
    return header + payload


def handle_connection(conn, my_host, set_name, outfile, lock):
    with conn:
        while True:
            msg = read_message(conn)
            if msg is None:
                return
            request_id, op_code, body = msg
            try:
                cmd = parse_command(op_code, body)
            except Exception:
                return
            name = command_name(cmd)
            # Log the full command document (JSON) so the test can see exactly what the connecting
            # node sends -- in particular any saslStart and which mechanism/payload it carries.
            with lock:
                with open(outfile, "a") as f:
                    f.write(name + "\t" + repr(cmd) + "\n")
                    f.flush()
            if name.lower() in ("hello", "ismaster"):
                internal_client = cmd.get("internalClient", {}) or {}
                max_wire_version = internal_client.get("maxWireVersion", 25)
                reply = build_hello_reply(my_host, set_name, max_wire_version)
            else:
                # Anything else (including a saslStart, which the fix must prevent) just gets an
                # error so the connecting node tears the connection down.
                reply = {"ok": 0.0, "errmsg": "forged peer", "code": 59}
            conn.sendall(frame_reply(request_id, op_code, reply))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--outfile", type=str, required=True)
    parser.add_argument("--set-name", type=str, required=True)
    args = parser.parse_args()

    my_host = f"127.0.0.1:{args.port}"
    # Truncate the output file at startup.
    open(args.outfile, "w").close()

    lock = threading.Lock()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(16)

    while True:
        conn, _ = server.accept()
        threading.Thread(
            target=handle_connection,
            args=(conn, my_host, args.set_name, args.outfile, lock),
            daemon=True,
        ).start()


if __name__ == "__main__":
    main()
