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

# A tiny, dependency-free BSON codec. The peer only needs to decode the handful of command
# documents the connecting node sends during the handshake and encode simple reply documents, so
# rather than depend on the external 'bson' module (which is not available in every test Python
# environment, e.g. Windows) we implement just enough of BSON here.


def bson_encode(doc):
    out = b""
    for key, value in doc.items():
        out += _bson_encode_element(key, value)
    return struct.pack("<i", len(out) + 5) + out + b"\x00"


def _bson_encode_element(key, value):
    name = key.encode("utf-8") + b"\x00"
    # bool must be checked before int (bool is a subclass of int).
    if isinstance(value, bool):
        return b"\x08" + name + (b"\x01" if value else b"\x00")
    if isinstance(value, int):
        if -(2**31) <= value < 2**31:
            return b"\x10" + name + struct.pack("<i", value)
        return b"\x12" + name + struct.pack("<q", value)
    if isinstance(value, float):
        return b"\x01" + name + struct.pack("<d", value)
    if isinstance(value, str):
        encoded = value.encode("utf-8") + b"\x00"
        return b"\x02" + name + struct.pack("<i", len(encoded)) + encoded
    if isinstance(value, datetime.datetime):
        return b"\x09" + name + struct.pack("<q", int(value.timestamp() * 1000))
    if isinstance(value, (list, tuple)):
        return b"\x04" + name + bson_encode({str(i): v for i, v in enumerate(value)})
    if isinstance(value, dict):
        return b"\x03" + name + bson_encode(value)
    if value is None:
        return b"\x0a" + name
    raise TypeError(f"unsupported BSON type for key {key!r}: {type(value)!r}")


def bson_decode(data, offset=0):
    doc_len = struct.unpack_from("<i", data, offset)[0]
    end = offset + doc_len
    i = offset + 4
    out = {}
    while i < end - 1:
        elem_type = data[i]
        i += 1
        nul = data.index(b"\x00", i)
        key = data[i:nul].decode("utf-8")
        i = nul + 1
        if elem_type == 0x01:  # double
            out[key] = struct.unpack_from("<d", data, i)[0]
            i += 8
        elif elem_type == 0x02:  # string
            ln = struct.unpack_from("<i", data, i)[0]
            i += 4
            out[key] = data[i:i + ln - 1].decode("utf-8")
            i += ln
        elif elem_type == 0x03:  # embedded document
            sub_len = struct.unpack_from("<i", data, i)[0]
            out[key] = bson_decode(data, i)
            i += sub_len
        elif elem_type == 0x04:  # array
            sub_len = struct.unpack_from("<i", data, i)[0]
            sub = bson_decode(data, i)
            i += sub_len
            out[key] = [sub[k] for k in sorted(sub, key=int)]
        elif elem_type == 0x05:  # binary
            ln = struct.unpack_from("<i", data, i)[0]
            i += 4 + 1  # length + subtype byte
            out[key] = data[i:i + ln]
            i += ln
        elif elem_type == 0x07:  # ObjectId
            out[key] = data[i:i + 12]
            i += 12
        elif elem_type == 0x08:  # bool
            out[key] = data[i] != 0
            i += 1
        elif elem_type == 0x09:  # UTC datetime
            out[key] = struct.unpack_from("<q", data, i)[0]
            i += 8
        elif elem_type == 0x0A:  # null
            out[key] = None
        elif elem_type == 0x10:  # int32
            out[key] = struct.unpack_from("<i", data, i)[0]
            i += 4
        elif elem_type == 0x11:  # timestamp
            out[key] = struct.unpack_from("<Q", data, i)[0]
            i += 8
        elif elem_type == 0x12:  # int64
            out[key] = struct.unpack_from("<q", data, i)[0]
            i += 8
        else:
            raise ValueError(f"unhandled BSON type 0x{elem_type:02x} for key {key!r}")
    return out


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
        return bson_decode(body[offset:offset + doc_len])
    if op_code == OP_QUERY:
        # flags(int32), fullCollectionName(cstring), skip(int32), return(int32), query(BSON).
        offset = 4
        end = body.index(b"\x00", offset)
        offset = end + 1 + 8  # cstring terminator + skip + return
        doc_len = struct.unpack_from("<i", body, offset)[0]
        return bson_decode(body[offset:offset + doc_len])
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
    encoded = bson_encode(reply_doc)
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
