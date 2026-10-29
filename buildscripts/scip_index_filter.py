#!/usr/bin/env python3
"""Strip non-public paths out of a SCIP index before it is published.

The SCIP index is uploaded to a public S3 bucket, but `//src/...` builds targets that
Copybara never syncs to the public repo (notably `src/third_party/private/**`). Bazel has
no notion of that policy, so filter against the Copybara rules themselves and fail the
build if anything excluded survives.

The index is rewritten at the protobuf wire-format level rather than via generated
bindings: the top-level `scip.Index` message is a flat sequence of length-delimited
fields (1 = metadata, 2 = documents, 3 = external_symbols), so documents can be dropped
by streaming without materializing a multi-gigabyte message in memory.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import BinaryIO, Iterator

sys.path.append(str(Path(__file__).resolve().parents[1]))

from buildscripts.copybara.forbidden_text_check import glob_to_regex
from buildscripts.copybara.path_rules import load_copybara_path_rules

DEFAULT_RULES = Path("buildscripts/copybara/copybara_path_rules.json")

# Top-level scip.Index field numbers.
FIELD_METADATA = 1
FIELD_DOCUMENT = 2
FIELD_EXTERNAL_SYMBOL = 3

# scip.Document field number for relative_path.
DOCUMENT_RELATIVE_PATH = 1


def load_exclusions(rules_path: Path) -> list[re.Pattern[str]]:
    rules = load_copybara_path_rules(rules_path)
    return [glob_to_regex(pattern) for pattern in rules.common_files_to_exclude]


def is_excluded(path: str, exclusions: list[re.Pattern[str]]) -> bool:
    return any(pattern.match(path) for pattern in exclusions)


def read_varint(stream: BinaryIO) -> int | None:
    value = 0
    shift = 0
    while True:
        chunk = stream.read(1)
        if not chunk:
            return None
        byte = chunk[0]
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def iter_fields(stream: BinaryIO) -> Iterator[tuple[int, bytes]]:
    """Yield (field_number, payload) for each length-delimited top-level field."""
    while True:
        tag = read_varint(stream)
        if tag is None:
            return
        field_number, wire_type = tag >> 3, tag & 7
        if wire_type != 2:
            raise ValueError(f"unexpected wire type {wire_type} for field {field_number}")
        length = read_varint(stream)
        if length is None:
            raise ValueError(f"truncated length for field {field_number}")
        payload = stream.read(length)
        if len(payload) < length:
            raise ValueError(f"truncated payload for field {field_number}")
        yield field_number, payload


def document_relative_path(payload: bytes) -> str | None:
    """Pull relative_path out of a serialized scip.Document without full decoding."""
    offset = 0
    while offset < len(payload):
        tag = payload[offset]
        offset += 1
        field_number, wire_type = tag >> 3, tag & 7
        if wire_type != 2:
            return None
        length = 0
        shift = 0
        while True:
            byte = payload[offset]
            offset += 1
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        value = payload[offset : offset + length]
        offset += length
        if field_number == DOCUMENT_RELATIVE_PATH:
            return value.decode("utf-8", "replace")
    return None


def filter_compdb(input_path: Path, output_path: Path, exclusions: list[re.Pattern[str]]) -> int:
    entries = json.loads(input_path.read_text())
    kept = [entry for entry in entries if not is_excluded(entry["file"], exclusions)]
    dropped = len(entries) - len(kept)
    output_path.write_text(json.dumps(kept))
    print(f"compdb: kept {len(kept)}, dropped {dropped} of {len(entries)} entries")
    return dropped


def filter_index(input_path: Path, output_path: Path, exclusions: list[re.Pattern[str]]) -> int:
    kept_documents = 0
    dropped_documents = 0
    other_fields = 0
    with input_path.open("rb") as source, output_path.open("wb") as sink:
        for field_number, payload in iter_fields(source):
            if field_number == FIELD_DOCUMENT:
                path = document_relative_path(payload)
                if path is not None and is_excluded(path, exclusions):
                    dropped_documents += 1
                    continue
                kept_documents += 1
            else:
                other_fields += 1
            sink.write(encode_varint(field_number << 3 | 2))
            sink.write(encode_varint(len(payload)))
            sink.write(payload)
    print(
        f"index: kept {kept_documents} documents, dropped {dropped_documents}, "
        f"passed through {other_fields} non-document fields"
    )
    return dropped_documents


def verify_index(input_path: Path, exclusions: list[re.Pattern[str]]) -> list[str]:
    """Return excluded paths still present. Empty means the index is safe to publish."""
    offenders: list[str] = []
    with input_path.open("rb") as source:
        for field_number, payload in iter_fields(source):
            if field_number != FIELD_DOCUMENT:
                continue
            path = document_relative_path(payload)
            if path is not None and is_excluded(path, exclusions):
                offenders.append(path)
    return offenders


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rules", type=Path, default=DEFAULT_RULES)
    subparsers = parser.add_subparsers(dest="command", required=True)

    compdb = subparsers.add_parser("compdb", help="drop excluded entries from a compdb")
    compdb.add_argument("--input", type=Path, default=Path("compile_commands.json"))
    compdb.add_argument("--output", type=Path, default=Path("compile_commands.json"))

    index = subparsers.add_parser("index", help="drop excluded documents from a SCIP index")
    index.add_argument("--input", type=Path, default=Path("index.scip"))
    index.add_argument("--output", type=Path, default=Path("index.filtered.scip"))

    verify = subparsers.add_parser("verify", help="fail if excluded paths remain")
    verify.add_argument("--input", type=Path, default=Path("index.scip"))

    args = parser.parse_args()
    exclusions = load_exclusions(args.rules)

    if args.command == "compdb":
        filter_compdb(args.input, args.output, exclusions)
        return 0

    if args.command == "index":
        filter_index(args.input, args.output, exclusions)
        return 0

    offenders = verify_index(args.input, exclusions)
    if offenders:
        print(
            f"ERROR: {len(offenders)} non-public paths present in {args.input}; refusing to publish",
            file=sys.stderr,
        )
        for path in sorted(set(offenders))[:20]:
            print(f"  {path}", file=sys.stderr)
        return 1
    print(f"verify: no non-public paths in {args.input}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
