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

class test_prepare_preserve_prepare_base(wttest.WiredTigerTestCase):
    conn_config = 'precise_checkpoint=true,preserve_prepared=true,statistics=(all)'

    def get_stats(self, stats, uri, session):
        """Get the current values of multiple statistics."""
        stat_cursor = session.open_cursor('statistics:' + uri)
        results = {}
        for stat in stats:
            results[stat] = stat_cursor[stat][2]
        stat_cursor.close()
        return results

    def checkpoint_and_verify_stats(self, expected_changes, uri, session = None):
        if session is None:
            session = self.session

        stats_to_check = list(expected_changes.keys())
        old_stats = self.get_stats(stats_to_check, uri, session)

        session.checkpoint()

        new_stats = self.get_stats(stats_to_check, uri, session)

        for stat, expect_increase in expected_changes.items():
            diff = new_stats[stat] - old_stats[stat]
            if expect_increase:
                self.assertGreater(diff, 0,
                    f"Stat {stat}: expected increase, got diff {diff}")
            else:
                self.assertEqual(diff, 0,
                    f"Stat {stat}: expected no change, got diff {diff}")

        return new_stats
