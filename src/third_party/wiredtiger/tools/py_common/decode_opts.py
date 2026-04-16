#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any own all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

from dataclasses import dataclass
from typing import Any, Optional

@dataclass
class DecodeOptions:
    # --- Input routing ---
    # Input file is a hex dump embedded in log messages.
    dumpin: bool = False
    # Input is a full disagg table from the GetTableAtLSN endpoint.
    disagg_table: bool = False
    # Treat the input as a fragment with no WT file header.
    fragment: bool = False

    # --- Decode behavior ---
    # Input comes from disaggregated storage.
    disagg: bool = False
    # Skip reading/processing cell data (print headers only).
    skip_data: bool = False
    # Continue decoding past checksum failures instead of stopping.
    cont: bool = False

    # --- Output formatting ---
    # Show raw input bytes interleaved with decoded output.
    split: bool = False
    # Decode cell values as BSON and pretty-print them.
    bson: bool = False
    # Writable file-like object for CSV page-statistics output, or None.
    output: Any = None

    # --- Seek / page-limit ---
    # Byte offset in the file to start decoding from.
    offset: int = 0
    # Maximum number of pages to decode (0 = unlimited).
    pages: int = 0

    # --- Disagg ---
    # Path to the encryption key file used by the pagedecryptor tool.
    keyfile: Optional[str] = None
    # For SQLite input: decode only the record with this LSN.
    lsn: Optional[int] = None
    # For SQLite input: decode only pages for this page_id.
    page_id: Optional[int] = None
