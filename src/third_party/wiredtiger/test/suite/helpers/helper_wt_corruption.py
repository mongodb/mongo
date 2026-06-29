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
