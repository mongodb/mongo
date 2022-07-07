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

import os, wiredtiger, wttest
from helper_tiered import  TieredConfigMixin, gen_tiered_storage_sources, get_check
from wtscenario import make_scenarios

StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered07.py
#    Basic tiered storage API for schema operations.
class test_tiered07(wttest.WiredTigerTestCase, TieredConfigMixin):

    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered07', tiered_only=True)

    # FIXME-WT-8897 Disabled S3 (only indexing dirstore in storage sources) as S3 directory listing 
    # is interpreting a directory to end in a '/', whereas the code in the tiered storage doesn't 
    # expect that. Enable when fixed.
    # Make scenarios for different cloud service providers
    flush_obj = [('ckpt', dict(first_ckpt=True)),
                 ('no_ckpt', dict(first_ckpt=False)),
                ]
    tiered_storage_dirstore_source = storage_sources[:1]
    scenarios = make_scenarios(flush_obj, tiered_storage_dirstore_source)

    uri = "table:abc"
    uri2 = "table:ab"
    uri3 = "table:abcd"
    uri4 = "table:abcde"
    localuri = "table:local"
    newuri = "table:tier_new"

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)
    
    def conn_config(self):
        return TieredConfigMixin.conn_config(self)
        
    def check(self, tc, base, n):
        get_check(self, tc, 0, n)

    # Test calling schema APIs with a tiered table.
    def test_tiered(self):
        # Create a new tiered table.
        self.pr('create table')
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.pr('create table 2')
        self.session.create(self.uri2, 'key_format=S,value_format=S')
        self.pr('create table 3')
        self.session.create(self.uri3, 'key_format=S,value_format=S')
        self.pr('create table local')
        self.session.create(self.localuri, 'key_format=S,value_format=S,tiered_storage=(name=none)')

        # Rename is not supported for tiered tables.
        msg = "/is not supported/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.rename(self.uri, self.newuri, None), 0), msg)

        # Add some data and flush tier.
        self.pr('add one item to all tables')
        c = self.session.open_cursor(self.uri)
        c["0"] = "0"
        self.check(c, 0, 1)
        c.close()
        c = self.session.open_cursor(self.uri2)
        c["0"] = "0"
        self.check(c, 0, 1)
        c.close()
        c = self.session.open_cursor(self.uri3)
        c["0"] = "0"
        self.check(c, 0, 1)
        c.close()
        c = self.session.open_cursor(self.localuri)
        c["0"] = "0"
        c.close()
        if (self.first_ckpt):
            self.session.checkpoint()
        self.pr('After data, call flush_tier')
        self.session.flush_tier(None)
        if (not self.first_ckpt):
            self.session.checkpoint()

        # Drop table.
        self.pr('call drop')
        self.session.drop(self.localuri)
        self.session.drop(self.uri)

        # By default, the remove_files configuration for drop is true. This means that the
        # drop operation for tiered tables should both remove the files from the metadata
        # file and remove the corresponding local object files in the directory.
        self.assertFalse(os.path.isfile("abc-0000000001.wtobj"))
        self.assertFalse(os.path.isfile("abc-0000000002.wtobj"))

        # Dropping a table using the force setting should succeed even if the table does not exist.
        self.pr('drop with force')
        self.session.drop(self.localuri, 'force=true')
        self.session.drop(self.uri, 'force=true')

        # Dropping a table should not succeed if the table does not exist.
        # Test dropping a table that was previously dropped.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.localuri, None))
        # Test dropping a table that does not exist.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.drop("table:random_non_existent", None))

        # Create new table with same name. This should error.
        self.session.create(self.newuri, 'key_format=S')

        # If we didn't do a checkpoint before the flush_tier then creating with the same name
        # will succeed because no bucket objects were created. 
        if (self.first_ckpt):
            self.pr('check cannot create with same name')
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda:self.session.create(self.uri, 'key_format=S'))
        else:
            self.session.create(self.uri, 'key_format=S')

        # Make sure there was no problem with overlapping table names.
        self.pr('check original similarly named tables')
        c = self.session.open_cursor(self.uri2)
        self.check(c, 0, 1)
        c.close()
        c = self.session.open_cursor(self.uri3)
        self.check(c, 0, 1)
        c.close()

        # Create new table with new name.
        self.pr('create new table')
        self.session.create(self.newuri, 'key_format=S')

        # Test the drop operation without removing associated files.
        self.session.create(self.uri4, 'key_format=S,value_format=S')
        self.session.drop(self.uri4, 'remove_files=false')
        self.assertTrue(os.path.isfile("abcde-0000000001.wtobj"))

if __name__ == '__main__':
    wttest.run()
