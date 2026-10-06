#!/usr/bin/env python3
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

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin, range_inclusive
from wtscenario import make_scenarios

# Verify that a follower truncate obeys the commit timestamp requirement that every other write to
# a disaggregated table obeys. The range holds no key the follower itself wrote, so the truncate
# list entry is the only record of the operation.

@disagg_test_class
class test_layered_fast_truncate22(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader"),'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)

    scenarios = make_scenarios(disagg_storages)

    nitems = 100

    def test_untimestamped_follower_truncate_is_refused(self):
        keys = range_inclusive(1, self.nitems)
        self.setup_leader(keys=keys)
        self.setup_follower()

        start = self.session.open_cursor(self.uri)
        start.set_key(self.key(1))
        stop = self.session.open_cursor(self.uri)
        stop.set_key(self.key(self.nitems))
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)
        start.close()
        stop.close()

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(),
            '/commit timestamp is required for writes to disaggregated tables/')

        # The refused commit rolled the truncate back, so every key is still visible.
        self.assertEqual(self.visible_keys(), list(keys))
