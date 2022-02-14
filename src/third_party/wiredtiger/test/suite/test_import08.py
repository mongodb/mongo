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
# test_import08.py
# Check that transaction ids from imported files are ignored regardless of write generation.

import os, re, shutil
from test_import01 import test_import_base
from wtscenario import make_scenarios

class test_import08(test_import_base):
    conn_config = 'cache_size=50MB,log=(enabled)'

    original_db_file = 'original_db_file'
    uri = 'file:' + original_db_file

    nrows = 100
    ntables = 10
    keys = [b'1', b'2', b'3', b'4', b'5', b'6']
    values = [b'\x01\x02aaa\x03\x04', b'\x01\x02bbb\x03\x04', b'\x01\x02ccc\x03\x04',
              b'\x01\x02ddd\x03\x04', b'\x01\x02eee\x03\x04', b'\x01\x02fff\x03\x04']
    ts = [10*k for k in range(1, len(keys)+1)]
    create_config = 'allocation_size=512,key_format=u,log=(enabled=true),value_format=u'
    scenarios = make_scenarios([
        ('file_metadata', dict(repair=False)),
        ('repair', dict(repair=True)),
    ])

    def parse_write_gen(self, config):
        # The search string will look like: 'write_gen=<num>'.
        # Just reverse the string and take the digits from the back until we hit '='.
        write_gen = re.search("write_gen=\d+", config)
        self.assertTrue(write_gen is not None)
        write_gen_str = str()
        for c in reversed(write_gen.group(0)):
            if not c.isdigit():
                self.assertEqual(c, '=')
                break
            write_gen_str = c + write_gen_str
        return int(write_gen_str)

    def test_import_write_gen(self):
        # Make a bunch of files and fill them with data. This has the side effect of allocating a
        # lot of transaction ids which is important for our test.
        self.populate(self.ntables, self.nrows)

        # Find the URI of one of the generated tables.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor('metadata:')
        generated_uri = None
        for k, _ in cursor2:
            if k.startswith('table:'):
                generated_uri = k
                break
        cursor2.close()

        # Now begin a transaction and remove the first entry in the table.
        #
        # This is going to allocate a transaction ID and more importantly, will ensure that pinned
        # ID is set for subsequent checkpoints in this test. If we don't do this, reconciliation
        # won't actually write transaction IDs to the disk since they will be deemed obsolete.
        session2.begin_transaction()
        self.assertTrue(generated_uri is not None)
        cursor2 = session2.open_cursor(generated_uri)
        cursor2.set_key(0)
        cursor2.remove()

        # Now create the file that we'll be importing later.
        self.session.create(self.uri, self.create_config)

        # Insert records into our newly created file.
        #
        # Since we allocated a bunch of transaction IDs before, we'll be allocating pretty high
        # IDs at this point.
        for i in range(0, len(self.keys)):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])

            # We want to checkpoint after each write to increase the write gen for this particular
            # btree.
            self.session.checkpoint()

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[self.uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)

        # Now that we've finished doing our checkpoints, we can let go of the transaction ID we
        # allocated earlier.
        session2.rollback_transaction()

        # Close the connection.
        self.close_conn()

        # Create a new database and connect to it.
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

        # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[-1]))

        # Copy over the datafiles for the object we want to import.
        self.copy_file(self.original_db_file, '.', newdir)

        # Contruct the config string.
        if self.repair:
            import_config = 'import=(enabled,repair=true)'
        else:
            import_config = 'import=(enabled,repair=false,file_metadata=(' + \
                original_db_file_config + '))'

        # Import the file.
        self.session.create(self.uri, import_config)

        # Verify object.
        self.verifyUntilSuccess(self.session, self.uri, None)

        # Check the write generation of the new table.
        #
        # The important thing to check is that it is greater than 1 (the current connection-wide
        # base write gen).
        c = self.session.open_cursor('metadata:')
        original_db_file_config = c[self.uri]
        c.close()
        write_gen = self.parse_write_gen(original_db_file_config)
        self.printVerbose(3, 'IMPORTED WRITE GEN: {}'.format(write_gen))
        self.assertGreater(write_gen, 1)

        # Check that the values are all visible.
        #
        # These values were written in the previous database with HIGH transaction IDs. Since we're
        # on a fresh connection, our IDs are starting back from 1 so we MUST wipe the IDs otherwise
        # they won't be visible to us and this test will fail.
        #
        # Since we were checkpointing after each write, the write gen of some of these pages will
        # definitely be higher than 1.
        #
        # If we use the old scheme and compare the page's write gen with the connection-wide base
        # write gen (which will be 1 since we made a new database), then we won't see some records
        # and the test will fail.
        #
        # If we use the new scheme and compare with the btree specific base write gen that was
        # supplied in the metadata, we will realise that it was from the previous run and wipe the
        # IDs meaning that all records will be visible.
        self.check(self.uri, self.keys, self.values)

        # Perform a checkpoint.
        self.session.checkpoint()
