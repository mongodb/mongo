#!/usr/bin/env python
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
# of this software dedicate any and all copyright interest in the
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

# Helpers for tests that drive the wt utility against (possibly corrupted)
# btree files.

import re

from helper import WiredTigerCursor
from helper_disagg import DisaggCorruptionMixin
from metadata_helper import get_table_id
from suite_subprocess import suite_subprocess


# Default byte pattern used to clobber a leaf or root page. The repeat is wide
# enough that any reasonable check_size lands entirely inside the pattern.
DEFAULT_CORRUPT_PATTERN = b'\xde\xad\xbe\xef' * 16


def corrupt_btree_file_at(path, offset, pattern=DEFAULT_CORRUPT_PATTERN):
    # Skip past the 64-byte block header so the checksum mismatch fires on the
    # page payload rather than on header bytes that callers special-case.
    with open(path, 'r+b') as f:
        f.seek(offset + 64)
        f.write(pattern)


# On-disk layout constants from btmem.h and cell.h. A block written with
# checksum=off only sums its first WT_BLOCK_COMPRESS_SKIP bytes, so a byte past
# that prefix can change without invalidating the checksum.
WT_BLOCK_COMPRESS_SKIP = 64
WT_PAGE_HEADER_BYTE_SIZE = 40
WT_CELL_SECOND_DESC = 0x08
WT_CELL_64V = 0x04
WT_CELL_ADDR_INT = 0x10
WT_CELL_ADDR_LEAF = 0x20
_WT_CELL_ADDR_TYPES = (0x00, 0x10, 0x20, 0x30, 0xd0)
_WT_CELL_KEY_TYPES = (0x50, 0x70)


def _wt_vunpack_uint(page, pos):
    # Decode a non-negative integer packed by __wt_vpack_uint.
    marker = page[pos]
    if marker & 0xc0 == 0x80:
        return (marker & 0x3f, pos + 1)
    if marker & 0xe0 == 0xc0:
        return ((((marker & 0x1f) << 8) | page[pos + 1]) + 64, pos + 2)
    if marker & 0xf0 == 0xe0:
        length = marker & 0x0f
        return (int.from_bytes(page[pos + 1:pos + 1 + length], 'big') + 8256, pos + 1 + length)
    raise AssertionError('unexpected packed integer marker 0x{:02x}'.format(marker))


def row_internal_page_cells(page):
    # Walk a row-store internal page's cells like __wt_cell_unpack_safe, returning
    # (offset, raw cell type) pairs. Must end on the page's in-memory size.
    mem_size = int.from_bytes(page[16:20], 'little')
    entries = int.from_bytes(page[20:24], 'little')
    pos = WT_PAGE_HEADER_BYTE_SIZE
    cells = []
    while pos < mem_size:
        desc = page[pos]
        raw = desc & 0x03 if desc & 0x03 else desc & 0xf0
        if raw in (0x01, 0x03):
            length = 1 + (desc >> 2)
        elif raw == 0x02:
            length = 2 + (desc >> 2)
        else:
            cur = pos + 1
            if raw == 0x70:
                cur += 1
            if raw in _WT_CELL_ADDR_TYPES and desc & WT_CELL_SECOND_DESC:
                flags = page[cur]
                cur += 1
                # Time aggregate fields, in __cell_unpack_addr_cell order.
                for bit in (0x08, 0x20, 0x02, 0x10, 0x40, 0x04):
                    if flags & bit:
                        _, cur = _wt_vunpack_uint(page, cur)
            if desc & WT_CELL_64V:
                _, cur = _wt_vunpack_uint(page, cur)
            size, cur = _wt_vunpack_uint(page, cur)
            if raw in _WT_CELL_KEY_TYPES:
                size += 64
            length = cur - pos + size
        cells.append((pos, raw))
        pos += length
    if pos != mem_size or len(cells) != entries:
        raise AssertionError('internal page walk ended at {} of {} with {} of {} cells'.format(
            pos, mem_size, len(cells), entries))
    return cells


def forge_internal_child_address_as_leaf(path, pages):
    # Relabel an internal child's address cell as a leaf, so the reference says
    # leaf while the child page stays internal. Picks a cell outside the
    # checksum bytes, which requires checksum=off.
    with open(path, 'r+b') as f:
        for offset, size in pages:
            f.seek(offset)
            page = f.read(size)
            for cell_offset, raw in row_internal_page_cells(page):
                if raw == WT_CELL_ADDR_INT and cell_offset >= WT_BLOCK_COMPRESS_SKIP:
                    f.seek(offset + cell_offset)
                    f.write(bytes([(page[cell_offset] & 0x0f) | WT_CELL_ADDR_LEAF]))
                    return
    raise AssertionError('no internal-child address cell outside the bytes the checksum covers')


# `wt verify -d dump_address` output differs between attached storage and
# disaggregated storage: ASC prints "[0: <offset>-<end>" addresses, disagg
# prints "page_id: N, disagg_lsn: M" on leaf lines and "[N, ?, M" on the root.
_LEAF_ASC_RE = re.compile(r'address:\s*\[0:\s*(\d+)-\d+')
_LEAF_DISAGG_RE = re.compile(r'page_id:\s*(\d+),\s*disagg_lsn:\s*(\d+)')
_ROOT_ASC_RE = re.compile(r'>\s*addr:\s*\[0:\s*(\d+)-\d+')
_ROOT_DISAGG_RE = re.compile(r'>\s*addr:\s*\[\s*(\d+),\s*\d+,\s*(\d+)')


def parse_verify_leaves(stdout, disagg):
    leaves = []
    pattern = _LEAF_DISAGG_RE if disagg else _LEAF_ASC_RE
    for line in stdout.splitlines():
        if 'row-store leaf' not in line:
            continue
        m = pattern.search(line)
        if not m:
            continue
        if disagg:
            leaves.append((int(m.group(1)), int(m.group(2))))
        else:
            leaves.append(int(m.group(1)))
    return leaves


def parse_verify_root(stdout, disagg):
    lines = stdout.splitlines()
    pattern = _ROOT_DISAGG_RE if disagg else _ROOT_ASC_RE
    for i, line in enumerate(lines):
        if line.strip() != 'Root:' or i + 1 >= len(lines):
            continue
        m = pattern.search(lines[i + 1])
        if not m:
            continue
        if disagg:
            return (int(m.group(1)), int(m.group(2)))
        return int(m.group(1))
    return None


_INTERNAL_ASC_RE = re.compile(r'\[0:\s*(\d+)-(\d+)')


def parse_verify_internal_pages(stdout):
    # Return (offset, size) for the root and every row-store internal page in
    # attached-storage `wt verify -d dump_address` output, root first.
    pages = []
    lines = stdout.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == 'Root:' and i + 1 < len(lines):
            m = _INTERNAL_ASC_RE.search(lines[i + 1])
            if m:
                pages.append((int(m.group(1)), int(m.group(2)) - int(m.group(1))))
        elif 'row-store internal' in line:
            m = _INTERNAL_ASC_RE.search(line)
            if m:
                pages.append((int(m.group(1)), int(m.group(2)) - int(m.group(1))))
    return pages


class CorruptedBTree:
    LEAF = 'leaf'
    ROOT = 'root'

    def __init__(self, base, target, addr, disagg):
        self.base = base
        self.target = target
        self.addr = addr
        self._disagg = disagg

    @property
    def uri(self):
        return ('layered:' if self._disagg else 'table:') + self.base

    @property
    def stable_uri(self):
        return 'file:' + self.base + ('.wt_stable' if self._disagg else '.wt')

    @property
    def verify_target(self):
        if self._disagg:
            return 'layered:' + self.base
        return 'file:' + self.base + '.wt'


class WtCliMixin(suite_subprocess, DisaggCorruptionMixin):
    def _is_disagg(self):
        return 'disagg' in self.hook_names

    def _wt_extra_conn_config(self):
        return 'disaggregated=(role="follower",page_log=palite)' if self._is_disagg() else None

    def _run_wt(self, *args, expect_failure=False):
        cmd = list(args)
        extra = self._wt_extra_conn_config()
        if extra is not None:
            cmd = ['-C', extra] + cmd
        self.runWt(cmd, outfilename='wt.out', errfilename='wt.err', failure=expect_failure)
        with open('wt.out') as f:
            stdout = f.read()
        with open('wt.err') as f:
            stderr = f.read()
        return stdout, stderr

    def _run_wt_verify_dump_address(self, target_uri, disagg):
        stdout, _ = self._run_wt('verify', '-d', 'dump_address', target_uri)
        return (parse_verify_leaves(stdout, disagg), parse_verify_root(stdout, disagg))

    def corrupt_btree(self, base, *, target, key, value, nrows):
        if target not in (CorruptedBTree.LEAF, CorruptedBTree.ROOT):
            raise ValueError(f"target must be {CorruptedBTree.LEAF!r} or {CorruptedBTree.ROOT!r}")
        if nrows <= 0:
            raise ValueError(f"nrows must be positive, got {nrows}")

        # A prior disagg corruption closes the connection while it holds the
        # palite SQLite lock; reopen it before we try session-level ops here.
        if self.conn is None:
            self.open_conn()

        disagg = self._is_disagg()
        handle = CorruptedBTree(base, target, addr=None, disagg=disagg)

        self.session.create(handle.uri, 'key_format=S,value_format=S')
        with WiredTigerCursor(self.session, handle.uri) as c:
            for i in range(nrows):
                c[key(i)] = value(i)
        self.session.checkpoint()

        # Snapshot table_id before verify-dump closes the connection.
        table_id = get_table_id(self.session, handle.stable_uri) if disagg else None

        leaves, root = self._run_wt_verify_dump_address(handle.verify_target, disagg=disagg)

        if target == CorruptedBTree.LEAF:
            self.assertTrue(leaves)
            # Middle leaf so the first/last keys live on either side of the skip.
            handle.addr = leaves[len(leaves) // 2]
        else:
            self.assertIsNotNone(root)
            handle.addr = root

        if disagg:
            page_id, lsn = handle.addr
            self.corrupt_page_image_at(table_id, page_id, lsn)
        else:
            corrupt_btree_file_at(base + '.wt', handle.addr)

        return handle
