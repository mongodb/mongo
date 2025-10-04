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
#
# test_truncate02.py
#       session level operations on tables
#

from test_truncate01 import test_truncate_base
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
import wttest

# test_truncate_fast_delete
#       When deleting leaf pages that aren't in memory, we set transactional
# information in the page's WT_REF structure, which results in interesting
# issues.
# FIXME-WT-15430: Re-enable once disaggregated storage works with fast truncate tests.
@wttest.skip_for_hook("disagg", "fast truncate is not supported yet")
class test_truncate_fast_delete(test_truncate_base):
    name = 'test_truncate'
    nentries = 10000

    # Use a small page size and lots of keys because we want to create lots
    # of individual pages in the file.
    types = [
        ('file', dict(type='file:', config=\
            'allocation_size=512,leaf_page_max=512')),
        # FIXME-WT-15430 Re-enable the layered table scenario once disaggregated storage works with fast truncate tests.
        # Consider whether we need this scenario here if the scenario is already defined in the test truncate base test.
        # ('layered', dict(type='layered:', config=\
        #     'allocation_size=512,leaf_page_max=512'))
    ]

    # This is all about testing the btree layer, not the schema layer, test
    # files and ignore tables.
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
        ]

    # Overflow records force pages to be instantiated, blocking fast delete.
    overflow = [
        ('ovfl1', dict(overflow=False)),
        ('ovfl2', dict(overflow=True)),
        ]

    # Random reads and writes force pages to be instantiated and potentially
    # create update structures, blocking fast delete and changing how fast
    # delete rollback works.
    reads = [
        ('read1', dict(readafter=False,readbefore=False)),
        ('read2', dict(readafter=True,readbefore=False)),
        ('read3', dict(readafter=False,readbefore=True)),
        ('read4', dict(readafter=True,readbefore=True)),
        ]
    writes = [
        ('write1', dict(writeafter=False,writebefore=False)),
        ('write2', dict(writeafter=True,writebefore=False)),
        ('write3', dict(writeafter=False,writebefore=True)),
        ('write4', dict(writeafter=True,writebefore=True)),
        ]

    # Test both commit and abort of the truncate transaction.
    txn = [
        ('txn1', dict(commit=True)),
        ('txn2', dict(commit=False)),
        ]

    scenarios = make_scenarios(types, keyfmt, overflow, reads, writes, txn,
                               prune=20, prunelong=1000)

    # Return the number of records visible to the cursor; test both forward
    # and backward iteration, they are different code paths in this case.
    def cursor_count(self, cursor, expected):
        count = 0
        while cursor.next() == 0:
            count += 1
        self.assertEqual(count, expected)
        cursor.reset()
        count = 0
        while cursor.prev() == 0:
            count += 1
        self.assertEqual(count, expected)

    # Open a cursor in a new session and confirm how many records it sees.
    def outside_count(self, ds, isolation, expected):
        s = self.conn.open_session()
        s.begin_transaction(isolation)
        cursor = ds.open_cursor(self.type + self.name, None, session=s)
        self.cursor_count(cursor, expected)
        s.close()

    # Trigger fast delete and test cursor counts.
    def test_truncate_fast_delete(self):
        uri = self.type + self.name

        '''
        print '===== run:'
        print 'config:', self.config + self.keyfmt, \
            'overflow=', self.overflow, \
            'readafter=', self.readafter, 'readbefore=', self.readbefore, \
            'writeafter=', self.writeafter, 'writebefore=', self.writebefore, \
            'commit=', self.commit
        '''

        # Create the object.
        ds = SimpleDataSet(self, uri, self.nentries,
                           config=self.config, key_format=self.keyfmt)
        ds.populate()

        # Optionally add a few overflow records so we block fast delete on
        # those pages.
        if self.overflow:
            cursor = ds.open_cursor(uri, None, 'overwrite=false')
            for i in range(1, self.nentries, 3123):
                cursor.set_key(ds.key(i))
                cursor.set_value(ds.value(i))
                cursor.update()
            cursor.close()

        # FIXME-WT-14977 Remove the conditional for layered tables once disaggregated storage can handle checkpoint id after restart.
        if self.type != 'layered:':
            self.session.checkpoint()
        # Close and re-open it so we get a disk image, not an insert skiplist.
            self.reopen_conn()

        # Optionally read/write a few rows before truncation.
        if self.readbefore or self.writebefore:
            cursor = ds.open_cursor(uri, None, 'overwrite=false')
            if self.readbefore:
                    for i in range(1, self.nentries, 737):
                        cursor.set_key(ds.key(i))
                        cursor.search()
            if self.writebefore:
                    for i in range(1, self.nentries, 988):
                        cursor.set_key(ds.key(i))
                        cursor.set_value(ds.value(i + 100))
                        cursor.update()
            cursor.close()

        # Begin a transaction, and truncate a big range of rows.
        self.session.begin_transaction(None)
        start = ds.open_cursor(uri, None)
        start.set_key(ds.key(10))
        end = ds.open_cursor(uri, None)
        end.set_key(ds.key(self.nentries - 10))
        ds.truncate(None, start, end, None)
        start.close()
        end.close()

        # Optionally read/write a few rows after truncation.
        if self.readafter or self.writeafter:
            cursor = ds.open_cursor(uri, None, 'overwrite=false')
            if self.readafter:
                    for i in range(1, self.nentries, 1123):
                        cursor.set_key(ds.key(i))
                        cursor.search()
            if self.writeafter:
                    for i in range(1, self.nentries, 621):
                        cursor.set_key(ds.key(i))
                        cursor.set_value(ds.value(i + 100))
                        cursor.update()
            cursor.close()

        # A cursor involved in the transaction should see the deleted records.
        # The number 19 comes from deleting row 10 (inclusive), to row N - 10,
        # exclusive, or 9 + 10 == 19.
        remaining = 19
        cursor = ds.open_cursor(uri, None)
        self.cursor_count(cursor, remaining)
        cursor.close()

        # A separate, read_committed cursor should not see the deleted records.
        self.outside_count(ds, "isolation=read-committed", self.nentries)

        # A separate, read_uncommitted cursor should see the deleted records.
        self.outside_count(ds, "isolation=read-uncommitted", remaining)

        # Commit/rollback the transaction.
        if self.commit:
                self.session.commit_transaction()
        else:
                self.session.rollback_transaction()

        # Check a read_committed cursor sees the right records.
        cursor = ds.open_cursor(uri, None)
        if self.commit:
                self.cursor_count(cursor, remaining)
        else:
                self.cursor_count(cursor, self.nentries)
        cursor.close()
