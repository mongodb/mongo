#! /usr/bin/env python3
"""Sends hand-built OP_MSG requests to a server and records how it responds.

The mongo shell can only send well-formed OP_MSG requests, so tests that exercise the server's
OP_MSG validation use this script to put arbitrary bytes on the wire.

The --spec file is a JSON document of the form {"steps": [step, ...]}. All steps run in order on a
single connection. Each step describes one request:

    {
        "flags": <uint32 flagBits, default 0>,
        "sections": [section, ...],
        "checksum": "valid" | "invalid" (optional; appends a CRC-32C, the caller sets the flag bit),
        "replies": <number of replies to read, default 1; 0 for fire-and-forget requests>,
        "injectTopologyVersion": <bool; copies the topologyVersion of the last reply carrying one
                                  into the kind 0 section, for awaitable hello>,
        "padMessageTo": <pads the kind 0 section so the whole message is exactly this many bytes>
    }

A section is one of:

    {"kind": 0 | 3, "doc": {...}}                     Body or telemetry context section.
    {"kind": 1, "name": "x", "docs": [{...}, ...],    Document sequence. "size" overrides the
     "size": <optional int32>}                        encoded size field.
    {"kind": <int>, "hex": "..."}                     Kind byte followed by raw bytes.
    {"hex": "..."}                                    Raw bytes with no kind byte.

A document containing {"$padTo": N} is padded with a string field so that it encodes to exactly N
bytes.

The --out file receives {"steps": [result, ...]} as relaxed extended JSON, where each result is:

    {
        "requestId": <int>,
        "replies": [{"requestId", "responseTo", "flags", "doc"}, ...],
        "closed": <bool; the server closed the connection before sending the expected replies>,
        "timedOut": <bool; no reply arrived within --timeout seconds>
    }

Once the server closes the connection, the remaining steps are not sent and are reported as closed.
"""

import argparse
import enum
import json
import socket
import struct
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from typing import Any, Self

import bson
from bson import json_util

OP_MSG = 2013
HEADER_FMT = "<iiii"
HEADER_LEN = struct.calcsize(HEADER_FMT)
CHECKSUM_PRESENT = 1 << 0
CHECKSUM_LEN = 4

BODY_KIND = 0
DOC_SEQUENCE_KIND = 1

STEPS_FIELD = "steps"
TOPOLOGY_VERSION_FIELD = "topologyVersion"
PAD_FIELD = "pad"
# A string element costs: type byte + field name + NUL + int32 length + contents + NUL.
PAD_OVERHEAD = 1 + len(PAD_FIELD) + 1 + 4 + 1


def _make_crc32c_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ 0x82F63B78 if crc & 1 else crc >> 1
        table.append(crc)
    return table


CRC32C_TABLE = _make_crc32c_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc = CRC32C_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


@dataclass(frozen=True)
class Document:
    """A BSON document, padded with a string field to encode to exactly `pad_to` bytes if set."""

    fields: dict[str, Any]
    pad_to: int | None = None

    @classmethod
    def from_json(cls, doc: dict[str, Any]) -> Self:
        fields = dict(doc)
        return cls(fields, fields.pop("$padTo", None))

    def encode(self) -> bytes:
        if self.pad_to is None:
            return bson.encode(self.fields)
        pad_len = self.pad_to - len(bson.encode(self.fields)) - PAD_OVERHEAD
        assert pad_len >= 0, f"cannot pad document to {self.pad_to} bytes"
        encoded = bson.encode({**self.fields, PAD_FIELD: "x" * pad_len})
        assert len(encoded) == self.pad_to
        return encoded


@dataclass(frozen=True)
class DocumentSection:
    """A kind 0 (body) or kind 3 (telemetry context) section."""

    kind: int
    doc: Document

    def encode(self) -> bytes:
        return bytes([self.kind]) + self.doc.encode()


@dataclass(frozen=True)
class DocumentSequenceSection:
    name: str
    docs: list[Document]
    size: int | None = None  # Overrides the encoded size field.

    def encode(self) -> bytes:
        payload = self.name.encode() + b"\x00" + b"".join(doc.encode() for doc in self.docs)
        size = len(payload) + 4 if self.size is None else self.size
        return bytes([DOC_SEQUENCE_KIND]) + struct.pack("<i", size) + payload


@dataclass(frozen=True)
class RawSection:
    kind: int | None  # None for raw bytes with no kind byte.
    data: bytes

    def encode(self) -> bytes:
        return (b"" if self.kind is None else bytes([self.kind])) + self.data


Section = DocumentSection | DocumentSequenceSection | RawSection


def parse_section(section: dict[str, Any]) -> Section:
    kind = section.get("kind")
    if (data := section.get("hex")) is not None:
        return RawSection(kind, bytes.fromhex(data))
    if kind == DOC_SEQUENCE_KIND:
        docs = [Document.from_json(doc) for doc in section["docs"]]
        return DocumentSequenceSection(section["name"], docs, section.get("size"))
    return DocumentSection(kind, Document.from_json(section["doc"]))


def is_body(section: Section) -> bool:
    return isinstance(section, DocumentSection) and section.kind == BODY_KIND


def map_bodies(sections: list[Section], update: Callable[[Document], Document]) -> list[Section]:
    return [replace(s, doc=update(s.doc)) if is_body(s) else s for s in sections]


class Checksum(enum.StrEnum):
    VALID = "valid"
    INVALID = "invalid"


@dataclass(frozen=True)
class Step:
    flags: int
    sections: list[Section]
    checksum: Checksum | None
    replies: int
    inject_topology_version: bool
    pad_message_to: int | None

    @classmethod
    def from_json(cls, step: dict[str, Any]) -> Self:
        checksum = step.get("checksum")
        return cls(
            flags=step.get("flags", 0),
            sections=[parse_section(section) for section in step.get("sections", [])],
            checksum=None if checksum is None else Checksum(checksum),
            replies=step.get("replies", 1),
            inject_topology_version=step.get("injectTopologyVersion", False),
            pad_message_to=step.get("padMessageTo"),
        )


@dataclass(frozen=True)
class Reply:
    request_id: int
    response_to: int
    flags: int
    doc: dict[str, Any] | None

    def to_json(self) -> dict[str, Any]:
        return {
            "requestId": self.request_id,
            "responseTo": self.response_to,
            "flags": self.flags,
            "doc": self.doc,
        }


@dataclass
class StepResult:
    request_id: int
    replies: list[Reply] = field(default_factory=list)
    closed: bool = False
    timed_out: bool = False

    def to_json(self) -> dict[str, Any]:
        return {
            "requestId": self.request_id,
            "replies": [reply.to_json() for reply in self.replies],
            "closed": self.closed,
            "timedOut": self.timed_out,
        }


def build_message(step: Step, request_id: int, topology_version: Any) -> bytes:
    sections = step.sections
    if step.inject_topology_version:
        assert topology_version is not None, "no topologyVersion seen yet"
        sections = map_bodies(
            sections,
            lambda doc: replace(
                doc, fields={**doc.fields, TOPOLOGY_VERSION_FIELD: topology_version}
            ),
        )

    def frame(sections: list[Section]) -> bytes:
        body = struct.pack("<I", step.flags) + b"".join(s.encode() for s in sections)
        checksum_len = 0 if step.checksum is None else CHECKSUM_LEN
        length = HEADER_LEN + len(body) + checksum_len
        msg = struct.pack(HEADER_FMT, length, request_id, 0, OP_MSG) + body
        if step.checksum is not None:
            crc = crc32c(msg)
            if step.checksum is Checksum.INVALID:
                crc ^= 0xFFFFFFFF
            msg += struct.pack("<I", crc)
        return msg

    msg = frame(sections)
    if step.pad_message_to is not None:
        assert sum(map(is_body, sections)) == 1, "padding needs exactly one body section"
        missing = step.pad_message_to - len(msg)
        sections = map_bodies(
            sections, lambda doc: replace(doc, pad_to=len(doc.encode()) + missing)
        )
        msg = frame(sections)
        assert len(msg) == step.pad_message_to
    return msg


def recv_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_reply(sock) -> Reply | None:
    """Returns the decoded reply, or None if the server closed the connection."""
    header = recv_exactly(sock, HEADER_LEN)
    if header is None:
        return None
    length, request_id, response_to, op_code = struct.unpack(HEADER_FMT, header)
    body = recv_exactly(sock, length - HEADER_LEN)
    if body is None:
        return None
    assert op_code == OP_MSG, f"unexpected reply opcode {op_code}"

    (flags,) = struct.unpack_from("<I", body, 0)
    end = len(body) - (CHECKSUM_LEN if flags & CHECKSUM_PRESENT else 0)
    offset = 4
    doc = None
    while offset < end:
        kind = body[offset]
        offset += 1
        (size,) = struct.unpack_from("<i", body, offset)
        if kind == BODY_KIND:
            doc = bson.decode(body[offset : offset + size])
        offset += size
    return Reply(request_id, response_to, flags, doc)


def run_steps(host: str, port: int, steps: list[Step], timeout: float) -> list[StepResult]:
    results = []
    topology_version = None
    closed = False
    with socket.create_connection((host, port), timeout=timeout) as sock:
        for request_id, step in enumerate(steps, start=1):
            result = StepResult(request_id, closed=closed)
            results.append(result)
            if closed:
                continue

            try:
                sock.sendall(build_message(step, request_id, topology_version))
                for _ in range(step.replies):
                    reply = read_reply(sock)
                    if reply is None:
                        closed = True
                        break
                    result.replies.append(reply)
                    if reply.doc and TOPOLOGY_VERSION_FIELD in reply.doc:
                        topology_version = reply.doc[TOPOLOGY_VERSION_FIELD]
            except (BrokenPipeError, ConnectionResetError):
                closed = True
            except socket.timeout:
                result.timed_out = True
            result.closed = closed
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--spec", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()

    with open(args.spec) as f:
        steps = [Step.from_json(step) for step in json.load(f)[STEPS_FIELD]]
    results = run_steps(args.host, args.port, steps, args.timeout)
    with open(args.out, "w") as f:
        output = {STEPS_FIELD: [result.to_json() for result in results]}
        f.write(json_util.dumps(output, json_options=json_util.RELAXED_JSON_OPTIONS))


if __name__ == "__main__":
    main()
