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
# test_tiered13.py
# Check that importing tiered tables returns an error.

import os, shutil, wiredtiger
from helper_tiered import get_conn_config, gen_tiered_storage_sources, TieredConfigMixin
from test_import01 import test_import_base
from wtscenario import make_scenarios
import wttest 

class test_tiered13(test_import_base, TieredConfigMixin):
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered13', tiered_only=True)
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered13-000000000'
    fileuri_base = 'file:' + base
    file1uri = fileuri_base + '1.wtobj'
    file2 = base + '2.wtobj'
    file2uri = fileuri_base + '2.wtobj'
    otherfile = 'other.wt'
    otheruri = 'file:' + otherfile
    uri = "table:test_tiered13"

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def conn_config(self):
        self.saved_conn = get_conn_config(self) + '),create'
        return self.saved_conn

    def test_tiered13(self):
        # Create a new tiered table.
        self.session.create(self.uri, 'key_format=S,value_format=S,')
        # Add first data. Checkpoint, flush and close the connection.
        c = self.session.open_cursor(self.uri)
        c["0"] = "0"
        c.close()
        # Use force to make sure the new object is created. Otherwise there is no
        # existing checkpoint yet and the flush will think there is no work to do.
        self.session.checkpoint('flush_tier=(enabled,force=true)')
        c = self.session.open_cursor(self.uri)
        c["1"] = "1"
        c.close()
        self.session.checkpoint()
        # We now have the second object existing, with data in it.

        # Set up for the test.
        # - Create the tiered table (above).
        # - Find the metadata for the current file: object.
        # - Set up a new database for importing.
        #
        # Testing import and tiered tables. All should error:
        # - Try to import via the table:uri.
        # - Try to import via the table:uri with the file object's metadata.
        # - Try to import via the file:uri.
        # - Try to import via the file:uri with the file object's metadata.
        # - Try to import via a renamed file:name.wt.
        # - Try to import via a renamed file:name.wt with the file object's metadata.

        # Export the metadata for the current file object 2.
        cursor = self.session.open_cursor('metadata:', None, None)
        for k, v in cursor:
            if k.startswith(self.file2uri):
                fileobj_config = cursor[k]
            if k.startswith(self.uri):
                table_config = cursor[k]
        cursor.close()
        self.close_conn()
        # Contruct the config strings.
        import_enabled = 'import=(enabled,repair=true)'
        import_meta = 'import=(enabled,repair=false,file_metadata=(' + \
            fileobj_config + '))'
        table_import_meta = table_config + ',import=(enabled,repair=false,file_metadata=(' + \
            fileobj_config + '))'

        # Set up the import database.
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        newbucket = os.path.join(newdir, self.bucket)
        if self.ss_name == 'dir_store':
            os.mkdir(newbucket)
        # It is tricky to work around the extension and connection bucket setup for
        # creating the new import directory that is tiered-enabled.
        ext = self.extensionsConfig()
        conn_params = self.saved_conn + ext
        self.conn = self.wiredtiger_open(newdir, conn_params)
        self.session = self.setUpSessionOpen(self.conn)

        # Copy the file to the file names we're going to test later.
        self.copy_file(self.file2, '.', newdir)
        copy_from = self.file2
        copy_to = os.path.join(newdir, self.otherfile)
        shutil.copy(copy_from, copy_to)

        msg = '/Operation not supported/'
        enoent = '/No such file/'
        invalid = "/import for tiered storage is incompatible with the 'file_metadata' setting/"
        # Try to import via the table:uri. This fails with ENOENT because
        # it is looking for the normal on-disk file name. It cannot tell it
        # is a tiered table in this case.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, import_enabled), enoent)
        # Try to import via the table:uri with file metadata.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, table_import_meta), invalid)
        # Try to import via the file:uri.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.file2uri, import_enabled), msg)
        # Try to import via the file:uri with file metadata.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.file2uri, import_meta), invalid)

        # Try to import via a renamed object. If we don't send in metadata,
        # we cannot tell it was a tiered table until we read in the root page.
        # Only test this in diagnostic mode which has an assertion.
        #
        # FIXME-8644 There is an error path bug in wt_bm_read preventing this from
        # working correctly although the code to return an error is in the code.
        # Uncomment these lines when that bug is fixed.

        #if wiredtiger.diagnostic_build():
        #    self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
        #        lambda: self.session.create(self.otheruri, import_enabled), msg)

        # Try to import via a renamed object with metadata.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.otheruri, import_meta), invalid)
