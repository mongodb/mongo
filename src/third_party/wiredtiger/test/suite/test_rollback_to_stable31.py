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

from test_rollback_to_stable01 import test_rollback_to_stable_base
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable31.py
# Check what happens with RTS if you never set the stable timestamp.

class test_rollback_to_stable31(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]
    checkpoint_modes = [
        ('no-checkpoint', dict(checkpoint=False)),
        ('checkpoint', dict(checkpoint=True)),
    ]
    rollback_modes = [
        ('runtime', dict(crash=False)),
        ('recovery', dict(crash=True)),
    ]

    scenarios = make_scenarios(format_values, checkpoint_modes, rollback_modes)

    def test_rollback_to_stable(self):
        nrows = 10

        # Create a table without logging.
        uri = "table:rollback_to_stable31"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 10
            value_b = "bbbbb" * 10
            value_c = "ccccc" * 10

        # Do not set stable. (Don't set oldest either as it can't be later than stable.)

        # Write aaaaaa to all the keys at time 10.
        self.large_updates(uri, value_a, ds, nrows, False, 10)

        # Write bbbbbb to all the keys at time 20.
        self.large_updates(uri, value_b, ds, nrows, False, 20)

        # Write cccccc to all the keys at time 30.
        self.large_updates(uri, value_c, ds, nrows, False, 30)

        # Optionally checkpoint.
        if self.checkpoint:
            self.session.checkpoint()

        # Roll back, either via crashing or by explicit RTS.
        if self.crash:
            simulate_crash_restart(self, ".", "RESTART")
        else:
            self.conn.rollback_to_stable()

        if self.crash:
            if self.checkpoint:
                # Recovery-time RTS does nothing when no stable timestamp is set.
                self.check(0, uri, 0, nrows, 5)
                self.check(value_a, uri, nrows, 0, 15)
                self.check(value_b, uri, nrows, 0, 25)
                self.check(value_c, uri, nrows, 0, 35)
            else:
                # If we crashed without a checkpoint, everything should disappear entirely.
                self.check(0, uri, 0, 0, 5)
                self.check(value_a, uri, 0, 0, 15)
                self.check(value_b, uri, 0, 0, 25)
                self.check(value_c, uri, 0, 0, 35)
        else:
            # With an explicit runtime RTS we roll back to 0, but the end of the FLCS table
            # still moves forward.
            self.check(0, uri, 0, nrows, 5)
            self.check(value_a, uri, 0, nrows, 15)
            self.check(value_b, uri, 0, nrows, 25)
            self.check(value_c, uri, 0, nrows, 35)

if __name__ == '__main__':
    wttest.run()
