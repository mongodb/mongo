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

import os

from helper import simulate_crash_restart
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from test_rollback_to_stable01 import test_rollback_to_stable_base

# There's one message we need to see for the test to pass ("needle"), and
# a set of messages that are acceptable. Fail if a message is either
# unacceptable or we don't see the needle at all.
def custom_validator(data):
    acceptable = [
        "aborting update with txnid",
        "connection_logging_enabled",
        "deleted page walk skipped",
        "page with reconciled",
        "performing recovery rollback",
        "start rollback to stable",
        "performing shutdown rollback",
        "recovered checkpoint snapshot",
        "rolling back tree",
        "tree rolled back",
        "finished rollback to stable",
    ]
    needle = "skipped performing rollback to stable"

    found = False
    for line in data.splitlines():
        ok = False
        if needle in line:
            found = True
        for s in acceptable:
            if s in line:
                ok = True
                break
        if not ok and not found:
            raise Exception("Got unexpected message: {}".format(line))

    if not found:
        raise Exception('Failed to find "{}" in stdout'.format(needle))

# test_rollback_to_stable42.py
# Test that rollback to stable on a missing file complains and bails.
class test_rollback_to_stable42(test_rollback_to_stable_base):
    def conn_config(self):
        return 'verbose=(rts:1)'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_reopen_after_delete(self):
        if 'tiered' in self.hook_names:
            self.skipTest("We cannot reliably remove specific file names under tiered storage")
        if os.name == 'nt':
            self.skipTest("Windows doesn't like removing a file with another process using it")

        # RTS runs as part of shutdown, but it won't succeed since we
        # remove the file that needs rollback.
        self.ignoreTearDownLogs = True

        uri = 'table:test_rollback_to_stable42'
        nrows = 1000

        if self.value_format == '8t':
            value = 97
        else:
            value = 'a' * 10

        # Create our table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Save some unstable updates to disk.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.large_updates(uri, value, ds, nrows, False, 60)
        self.session.checkpoint()

        os.remove('test_rollback_to_stable42.wt')

        # RTS runs at shutdown and startup, and we need WiredTiger to
        # see that the file was deleted from under it (it might all be
        # in memory if we call conn.rollback_to_stable()). Kill both
        # of these birds with one stone.
        with self.customStdoutPattern(custom_validator):
            simulate_crash_restart(self, ".", "RESTART")

if __name__ == '__main__':
    wttest.run()
