#!/usr/bin/env python3
"""Regenerate the PEP 503 simple index for the mdb-build-public rapidyaml wheels.

Why this exists
---------------
Poetry (via `[[tool.poetry.source]] priority = "explicit"` in pyproject.toml)
fetches rapidyaml wheels and sdists from our S3-hosted PEP 503 simple index
at

    https://mdb-build-public.s3.amazonaws.com/rapidyaml_wheels/simple/

PEP 503 lets each anchor in the HTML index carry a `#sha256=<hex>` fragment
that installers (Poetry, pip, uv, etc.) can use to verify the download
without re-hashing locally. Our index historically shipped bare URLs with
no fragment, so every `poetry lock` had to re-download and hash each wheel
to populate the hashes in poetry.lock.

This script rebuilds the index with the fragments in place. After running
it once, `poetry lock` (or `uv lock` in the future) will pick the hashes
straight from the index without re-downloading, and future installers that
require indexes to carry hashes — for example rules_pycross's uv_translator,
which fails with `KeyError: 'hash'` on hashless lock entries — will work
against this bucket without extra plumbing.

Usage
-----
Requires AWS credentials with `s3:List/Get/Put` on the mdb-build-public
bucket. Runs against the bucket in-place.

    python3 buildscripts/mongo_rapidyaml_builds/regenerate_simple_index.py

The `--dry-run` flag prints the new HTML for BOTH the per-project index
and the root index to stdout without uploading — use this to verify the
anchors look right before overwriting the real files. Once verified,
drop the flag.

Writes four objects (two pairs, each pair is byte-identical):
  - `rapidyaml_wheels/simple/rapidyaml/` — trailing-slash key. Installers
    fetch `.../simple/rapidyaml/` (per PEP 503) and s3.amazonaws.com
    serves keys literally, so this exact key must exist. Same content
    as the mirror below.
  - `rapidyaml_wheels/simple/rapidyaml/index.html` — mirror for humans
    clicking through, and for future S3 static-website-hosting.
  - `rapidyaml_wheels/simple/` — trailing-slash root, listing project
    names. Rarely fetched by installers but mirrors the pattern.
  - `rapidyaml_wheels/simple/index.html` — same content as above.

Both `rapidyaml/` files carry `#sha256=<hex>` fragments on every anchor.

Reproducibility note
--------------------
The sha256s emitted here are computed by downloading each object's bytes
from S3 (with retries) and hashing locally. We deliberately do NOT trust
S3's `ETag` header, because for objects uploaded via multipart upload the
ETag is `md5(md5(part1)+md5(part2)+...)-<n>` — not a whole-object sha256
and not a whole-object md5. Downloading is cheap here (~few MB total).
"""

import argparse
import hashlib
import sys
import xml.etree.ElementTree as ET
from typing import Iterator

try:
    import boto3
    from botocore.exceptions import ClientError
except ImportError:
    sys.exit(
        "ERROR: boto3 is required. Install with `poetry install --with aws` "
        "(from the repo root) or `pip install boto3`."
    )

BUCKET = "mdb-build-public"
PREFIX = "rapidyaml_wheels/"
PROJECT = "rapidyaml"
# PEP 503 says an installer walks `simple/<normalized-project>/` for each
# package. Poetry/pip fetch that URL literally — no `index.html` suffix
# appended — so on plain s3.amazonaws.com (which doesn't route
# `some/path/` to `some/path/index.html` the way static-website endpoints
# do), we have to write the index file at the exact key that includes
# the trailing slash. S3 handles trailing-slash keys fine; they're just
# unusual to see from the AWS console UI.
PROJECT_INDEX_KEY = f"{PREFIX}simple/{PROJECT}/"
# Also mirror to `.../index.html` for humans who click through and for
# S3 static-website-hosting compatibility if that's ever enabled later.
PROJECT_INDEX_MIRROR_KEY = f"{PREFIX}simple/{PROJECT}/index.html"
# Same story for the root — installers point at `.../simple/` when the
# poetry source's `url` ends in a slash, so we write both.
ROOT_INDEX_KEY = f"{PREFIX}simple/"
ROOT_INDEX_MIRROR_KEY = f"{PREFIX}simple/index.html"
PUBLIC_URL_BASE = f"https://{BUCKET}.s3.amazonaws.com/{PREFIX}"


def list_wheel_and_sdist_keys(s3) -> Iterator[str]:
    """Yield object keys directly under PREFIX (not inside `simple/`).

    Skips the index HTML itself and any nested `simple/` content — those
    aren't distribution files.
    """
    paginator = s3.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=BUCKET, Prefix=PREFIX):
        for obj in page.get("Contents", []):
            key = obj["Key"]
            # Skip directory-marker objects, the index itself, and anything
            # nested under simple/ (that's index content, not artifacts).
            if key.endswith("/") or key.startswith(PREFIX + "simple/"):
                continue
            if not (key.endswith(".whl") or key.endswith(".tar.gz")):
                continue
            yield key


def sha256_of_key(s3, key: str) -> str:
    """Download the object and compute its sha256 in a streaming fashion.

    Uses a chunked streaming read via `Body.iter_chunks()` so we never
    hold the full wheel in memory (~10-50MB per manylinux wheel).
    """
    resp = s3.get_object(Bucket=BUCKET, Key=key)
    hasher = hashlib.sha256()
    for chunk in resp["Body"].iter_chunks(chunk_size=1 << 20):  # 1 MiB
        hasher.update(chunk)
    return hasher.hexdigest()


def render_project_index(entries: list[tuple[str, str]]) -> str:
    """Render a PEP 503-compliant per-project index page.

    This is the page at `simple/<project>/index.html` — the one Poetry/
    pip actually parse to enumerate downloadable files for `<project>`.
    Each anchor's href carries a `#sha256=<hex>` fragment so installers
    can verify the download without re-hashing.

    Args:
        entries: (filename, sha256_hex) pairs, sorted lexicographically.

    Returns:
        A minimal HTML5 document with one <a> per artifact.
    """
    # Build with ElementTree to guarantee well-formed HTML (all attrs
    # properly escaped even if a wheel filename ever contains oddities).
    root = ET.Element("html")
    head = ET.SubElement(root, "head")
    meta = ET.SubElement(head, "meta")
    meta.set("name", "pypi:repository-version")
    meta.set("content", "1.0")
    title = ET.SubElement(head, "title")
    title.text = f"Links for {PROJECT}"

    body = ET.SubElement(root, "body")
    for filename, sha in entries:
        # PEP 503 requires the anchor text to be the filename; the href
        # carries the hash fragment.
        anchor = ET.SubElement(body, "a")
        anchor.set("href", f"{PUBLIC_URL_BASE}{filename}#sha256={sha}")
        anchor.text = filename
        # A newline between anchors purely for human readability of the
        # generated HTML; browsers and index parsers ignore whitespace.
        anchor.tail = "\n"

    ET.indent(root, space="  ")
    return "<!DOCTYPE html>\n" + ET.tostring(root, encoding="unicode") + "\n"


def render_root_index() -> str:
    """Render the root `simple/index.html` — a bare list of project names.

    Only one project (rapidyaml) lives in this bucket, but PEP 503
    installers navigate `simple/` -> `simple/<project>/` -> file list.
    A human hitting `simple/` gets a reasonable landing page too.
    """
    root = ET.Element("html")
    head = ET.SubElement(root, "head")
    meta = ET.SubElement(head, "meta")
    meta.set("name", "pypi:repository-version")
    meta.set("content", "1.0")
    title = ET.SubElement(head, "title")
    title.text = "Simple index for mongodb rapidyaml wheels"

    body = ET.SubElement(root, "body")
    anchor = ET.SubElement(body, "a")
    anchor.set("href", f"{PROJECT}/")
    anchor.text = PROJECT
    anchor.tail = "\n"

    ET.indent(root, space="  ")
    return "<!DOCTYPE html>\n" + ET.tostring(root, encoding="unicode") + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the new index HTML to stdout without uploading.",
    )
    args = parser.parse_args()

    s3 = boto3.client("s3")

    print(f"Listing s3://{BUCKET}/{PREFIX}...", file=sys.stderr)
    keys = sorted(list_wheel_and_sdist_keys(s3))
    if not keys:
        sys.exit(f"ERROR: no wheels or sdists found under s3://{BUCKET}/{PREFIX}")
    print(f"  found {len(keys)} artifacts", file=sys.stderr)

    entries: list[tuple[str, str]] = []
    for key in keys:
        # Strip the PREFIX so the filename in the index is just the basename.
        filename = key[len(PREFIX) :]
        print(f"  hashing {filename}...", file=sys.stderr, end=" ", flush=True)
        sha = sha256_of_key(s3, key)
        print(sha[:12] + "...", file=sys.stderr)
        entries.append((filename, sha))

    project_html = render_project_index(entries)
    root_html = render_root_index()

    if args.dry_run:
        print(f"=== s3://{BUCKET}/{PROJECT_INDEX_KEY} ===")
        print(project_html)
        print(f"=== s3://{BUCKET}/{PROJECT_INDEX_MIRROR_KEY} (identical) ===")
        print(f"=== s3://{BUCKET}/{ROOT_INDEX_KEY} ===")
        print(root_html)
        print(f"=== s3://{BUCKET}/{ROOT_INDEX_MIRROR_KEY} (identical) ===")
        return

    def _put(key: str, body: str) -> None:
        print(f"Uploading to s3://{BUCKET}/{key}...", file=sys.stderr)
        s3.put_object(
            Bucket=BUCKET,
            Key=key,
            Body=body.encode("utf-8"),
            ContentType="text/html",
        )

    # Write to both the trailing-slash key (what Poetry/pip actually
    # fetch on plain s3.amazonaws.com) and the /index.html mirror (for
    # humans and future static-website-hosting).
    _put(PROJECT_INDEX_KEY, project_html)
    _put(PROJECT_INDEX_MIRROR_KEY, project_html)
    _put(ROOT_INDEX_KEY, root_html)
    _put(ROOT_INDEX_MIRROR_KEY, root_html)
    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except ClientError as e:
        sys.exit(f"S3 error: {e}")
