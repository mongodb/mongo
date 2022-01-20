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
# test_compat05.py
# Check log.archive compatibility API

import fnmatch, os, time
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet, simple_key
from wtscenario import make_scenarios

class test_compat05(wttest.WiredTigerTestCase, suite_subprocess):
    remove_values = [
        ('archive-false', dict(remove_arg = 'archive=false', removed = False)),
        ('archive-true', dict(remove_arg = 'archive=true', removed = True)),
        ('default', dict(remove_arg = '', removed = True)),
        ('remove-false', dict(remove_arg = 'remove=false', removed = False)),
        ('remove-override-order1', dict(remove_arg = 'archive=false,remove=true', removed = True)),
        ('remove-override-order2', dict(remove_arg = 'remove=true,archive=false', removed = True)),
        ('remove-true', dict(remove_arg = 'remove=true', removed = True)),
    ]
    scenarios = make_scenarios(remove_values)

    log1 = 'WiredTigerLog.0000000001'
    log2 = 'WiredTigerLog.0000000002'

    # Create the database with logging configured.
    def conn_config(self):
        return 'create,log=(enabled,file_max=100K,' + self.remove_arg + ')'

    # Check if the log file has been removed.
    def check_remove(self):
        removed = False
        for i in range(1,90):
            # Sleep and then see if log removal ran. We do this in a loop
            # for slow machines. Max out at 90 seconds.
            time.sleep(1.0)
            if not os.path.exists(self.log1):
                removed = True
                break
        return removed

    # Run a single test.
    def test_compat05(self):
        # Populate the database to create some log files.
        uri = 'table:test_compat05'
        ds = SimpleDataSet(self, uri, 10000, key_format='S', value_format='S')
        ds.populate()

        # Assert there's at least two log files so we can remove the first one.
        self.assertTrue(os.path.exists(self.log2))

        # Checkpoint
        self.session.checkpoint()

        # Assert the first log is there or not there.
        self.assertEquals(self.check_remove(), self.removed)

if __name__ == '__main__':
    wttest.run()
