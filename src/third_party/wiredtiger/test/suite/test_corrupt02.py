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

import wiredtiger, wttest
from metadata_helper import checkpoint_extent_list_blocks

# When a checkpoint drops a previous checkpoint, it reads that checkpoint's extent lists. If the
# extent-list block is corrupt, the read fails its checksum and drives the diagnostic extent-list
# dump path. That path used to fetch the btree's cached checkpoint list by reference, attach
# transient block manager state to it and then free it, which corrupted the shared list (a
# use-after-free), and it also tripped the checkpoint-validate diagnostics with a spurious
# "__assert_ckpt_matches" abort. With corruption_abort disabled the corrupt read should now surface
# as an ordinary WT_PANIC the application can handle, not a crash.
@wttest.skip_for_hook("tiered", "corrupts local block files not used by tiered storage")
@wttest.skip_for_hook("disagg", "corrupts blocks which are not relevant for disagg")
@wttest.skip_for_hook("parallel_checkpoint", "FIXME-WT-18134: deliberately fails a checkpoint; parallel checkpoint tears down worker threads mid-transaction")
class test_corrupt02(wttest.WiredTigerTestCase):
    conn_config = ('cache_size=50MB,statistics=(fast),debug_mode=(corruption_abort=false),'
                   'eviction_dirty_trigger=50,eviction_updates_trigger=50')

    test_name = __qualname__
    uri = f'table:{test_name}'
    tablename = f'{test_name}.wt'

    def write_rows(self, start, count):
        cursor = self.session.open_cursor(self.uri)
        for i in range(start, start + count):
            cursor[str(i)] = b'a' * 500
        cursor.close()

    def test_corrupt02(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')

        # Load enough data that the alloc extent list is large enough to be written as its own block.
        self.write_rows(1, 12000)
        self.session.checkpoint()

        # Corrupt every alloc extent-list block the checkpoint cookies point at, leaving the rest of
        # the file intact.
        blocks = checkpoint_extent_list_blocks(self.session, 'file:' + self.tablename, kinds=('alloc',))
        self.assertGreater(len(blocks), 0,
            "expected at least one on-disk alloc extent-list block to corrupt")
        with open(self.tablename, 'r+b') as f:
            for offset, size in blocks:
                f.seek(offset)
                f.write(b'Bad!' * (size // 4))

        # Dirty the tree and checkpoint again. The new checkpoint drops the previous one and reads
        # its now-corrupt alloc extent list. This must surface as a handled WT_PANIC.
        self.write_rows(12000, 1000)
        self.assertRaisesException(
            wiredtiger.WiredTigerError, lambda: self.session.checkpoint())

        # The connection has panicked; every subsequent call including close returns WT_PANIC.
        self.assertRaisesException(
            wiredtiger.WiredTigerError, lambda: self.close_conn())

        # The corrupt-block read logs the corruption on stderr and dumps the extent lists on stdout.
        self.ignoreStdoutPatternIfExists('extent list')
        self.ignoreStderrPatternIfExists('checksum error')
