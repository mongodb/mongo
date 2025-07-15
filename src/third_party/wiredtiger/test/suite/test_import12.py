
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
# test_import12.py
# Import a file into a running database using the dry-run method used
# by the MongoDB server. Add in checkpoints in there too:
# - Export the table.
# - Import to running database.
# - Export the table.
# - Create/import the table.
# - Drop table with 'remove_files=false'
# - Create/import the table.

import os, random, re, shutil, string
import wttest
from wiredtiger import WiredTigerError, wiredtiger_strerror, WT_ERROR
from test_import01 import test_import_base

# test_import12
class test_import12(test_import_base):
    alter_config1 = 'access_pattern_hint=none'
    alter_config2 = 'access_pattern_hint=random'
    conn_config = 'cache_size=50MB'
    original_db_file = 'original_db_file'
    new_db_file = 'new_db_file'
    uri = 'file:' + original_db_file
    new_uri = 'file:' + new_db_file

    max_ckpt = 2

    nrows = 100
    ntables = 1
    keys = [b'1', b'2', b'3', b'4', b'5', b'6']
    values = [b'\x01\x02aaa\x03\x04', b'\x01\x02bbb\x03\x04', b'\x01\x02ccc\x03\x04',
              b'\x01\x02ddd\x03\x04', b'\x01\x02eee\x03\x04', b'\x01\x02fff\x03\x04']
    ts = [10*k for k in range(1, len(keys)+1)]
    create_config = 'access_pattern_hint=none,allocation_size=512,key_format=u,value_format=u'

    def test_file_import(self):
        self.session.create(self.uri, self.create_config)

        # Add data and perform a checkpoint.
        min_idx = 0
        max_idx = len(self.keys) // 3
        for i in range(min_idx, max_idx):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()

        # Add more data and checkpoint again.
        min_idx = max_idx
        max_idx = 2*len(self.keys) // 3
        for i in range(min_idx, max_idx):
            self.update(self.uri, self.keys[i], self.values[i], self.ts[i])
        self.session.checkpoint()
        orig_min_idx = min_idx
        orig_max_idx = max_idx

        # Export the metadata for the table.
        c = self.session.open_cursor('metadata:', None, None)
        original_db_file_config = c[self.uri]
        c.close()

        self.printVerbose(3, '\nFile configuration:\n' + original_db_file_config)
        self.pr(original_db_file_config)

        # Close the connection.
        self.close_conn()

        # Create a new database and connect to it.
        newdir = 'IMPORT_DB'
        # Construct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            original_db_file_config + '))'
        import_config2 = 'import=(enabled,repair=false,panic_corrupt=false,file_metadata=(' + \
            original_db_file_config + '))'
        import_repair = 'import=(enabled,repair=true)'

        for ck in range(0, self.max_ckpt + 1):
            if ck % 2 == 0:
                alter_config = self.alter_config2
            else:
                alter_config = self.alter_config1
            self.pr("ck " + str(ck))
            self.pr("remove and remake new database tree")
            shutil.rmtree(newdir, ignore_errors=True)
            os.mkdir(newdir)
            self.pr("Open connection")
            self.conn = self.setUpConnectionOpen(newdir)
            self.pr("Open session")
            self.session = self.setUpSessionOpen(self.conn)

            # Make a bunch of files and fill them with data.
            self.pr("populate and checkpoint")
            self.populate(self.ntables, self.nrows)
            self.session.checkpoint()

            max_idx = orig_max_idx
            min_idx = orig_min_idx
            # Bring forward the oldest to be past or equal to the timestamps we'll be importing.
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.ts[max_idx]))

            # Copy over the datafiles for the object we want to import.
            copy_from = os.path.join('.', self.original_db_file)
            copy_to = os.path.join(newdir, self.new_db_file)
            shutil.copy(copy_from, copy_to)

            # Do a dry-run of the import. Create and then drop.
            # Import the file. Open and close a cursor because creating the table leaves it
            # open and the drop returns EBUSY.
            self.pr("Create import table " + self.new_uri)
            self.session.create(self.new_uri, import_config)
            self.session.checkpoint()
            for i in range(0, ck):
                # Perform a checkpoint. First one is a normal checkpoint.
                # Any additional checkpoints use force.
                self.pr("Checkpoint loop " + str(i))
                self.session.checkpoint()
                if i == 0:
                    self.session.checkpoint()
                else:
                    # !!!
                    # Doing a forced checkpoint as the second checkpoint causes the second
                    # create/import below to fail because having two checkpoints renders the
                    # original root page non-existent in the on-disk imported table that was
                    # created-with-import and then dropped but leave the file on disk.
                    self.session.checkpoint("force=true")
            self.pr("Alter table: " + alter_config)
            self.session.alter(self.new_uri, alter_config)
            self.pr("Checkpoint after alter")
            self.session.checkpoint()

            c = self.session.open_cursor('metadata:', None, None)
            latest_meta = c[self.new_uri]
            c.close()
            self.pr('latest_meta: ' + latest_meta)
            self.assertTrue(latest_meta.find(alter_config) != -1)

            # Drop the file but don't remove it.
            self.pr("Drop table")
            self.session.drop(self.new_uri, "checkpoint_wait=false,lock_wait=true,remove_files=false")

            # Import the file for real.
            # This may fail based on the checkpoints.
            self.pr("Create again with " + import_config2)
            try:
                err = self.session.create(self.new_uri, import_config2)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_ERROR) in str(e):
                    err = WT_ERROR
                else:
                    raise e

            # The last import, if it failed, may have produced a message, ignore it.
            # Note that this only appears to happen in the disaggregated branch.
            # It's unknown yet why it doesn't ever happen in develop.  FIXME-WT-14713.
            self.ignoreStderrPatternIfExists(r'failed to read .* bytes at offset')

            # If the second create attempt failed, try again with repair=true.
            # We expect success.
            if err == WT_ERROR:
                self.pr("Create again with " + import_repair)
                self.session.create(self.new_uri, import_repair)

            # Check we opened at the latest checkpoint. Check the alter change made
            # it to the checkpoint metadata in the file used for repair=true.
            # FIXME-WT-13639. Uncomment this.
            #c = self.session.open_cursor('metadata:', None, None)
            #repair_meta = c[self.new_uri]
            #c.close()
            #self.pr('repair_meta: ' + repair_meta)
            #self.assertTrue(repair_meta.find(alter_config) != -1)
            #self.assertEqual(latest_meta, repair_meta)

            # Verify object.
            self.pr("Verify imported table")
            self.verifyUntilSuccess(self.session, self.new_uri, None)

            # Check that the previously inserted values survived the import.
            self.check(self.new_uri, self.keys[:max_idx], self.values[:max_idx])

            # Add some data and check that the table operates as usual after importing.
            min_idx = max_idx
            max_idx = len(self.keys)
            for i in range(min_idx, max_idx):
                self.update(self.new_uri, self.keys[i], self.values[i], self.ts[i])
            self.check(self.new_uri, self.keys, self.values)
            self.pr("Close connection - end of loop")
            self.conn.close()

            self.ignoreStdoutPatternIfExists('extent list')
