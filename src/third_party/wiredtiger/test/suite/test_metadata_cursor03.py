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

import wttest
from wtscenario import make_scenarios

# test_metadata03.py
#    Test atomic schema operations on create.
class test_metadata03(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled)'
    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
        ('table-index', dict(uri='table:', use_cg=False, use_index=True)),
        ('table-simple', dict(uri='table:', use_cg=False, use_index=False)),
    ]
    scenarios = make_scenarios(types)

    # Count actual log records in the log. Log cursors walk the individual
    # operations of a transaction as well as the entire record. Skip counting
    # any individual commit operations and only count entire records.
    def count_logrecs(self):
        count = 0
        c = self.session.open_cursor('log:', None, None)
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # Only count whole records, which is when opcount is zero.
            # If opcount is not zero it is an operation of a commit.
            if keys[2] == 0:
                count += 1
        c.close()
        return count

    def verify_logrecs(self, origcnt):
        #
        # Walk through all the log and make sure that creating any table
        # only writes two log records to the log.  The two records are the
        # commit entry itself and the sync record for the metadata file.
        #
        count = self.count_logrecs()
        # To be re-enabled when WT-3965 is fixed.
        #self.assertTrue(count == origcnt + 2)

    # Test that creating and dropping tables does not write individual
    # log records.
    def test_metadata03_create(self):
        uri = self.uri + 'table0'
        create_params = 'key_format=i,value_format=S,'

        cgparam = ''
        if self.use_cg or self.use_index:
            cgparam = 'columns=(k,v),'
        if self.use_cg:
            cgparam += 'colgroups=(g0),'

        # Create main table.
        origcnt = self.count_logrecs()
        self.session.create(uri, create_params + cgparam)
        self.verify_logrecs(origcnt)
        # Add in column group or index tables.
        if self.use_cg:
            # Create.
            cgparam = 'columns=(v),'
            suburi = 'colgroup:table0:g0'
            origcnt = self.count_logrecs()
            self.session.create(suburi, cgparam)
            self.verify_logrecs(origcnt)

        if self.use_index:
            # Create.
            suburi = 'index:table0:i0'
            origcnt = self.count_logrecs()
            self.session.create(suburi, cgparam)
            self.verify_logrecs(origcnt)

        # Dropping the main table will also drop all index or column group tables.
        origcnt = self.count_logrecs()
        self.session.drop(uri)
        self.verify_logrecs(origcnt)

if __name__ == '__main__':
    wttest.run()
