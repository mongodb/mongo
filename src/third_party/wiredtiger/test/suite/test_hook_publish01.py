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

import wiredtiger
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios


@wttest.only_for_hook("disagg", "checks the hook's schema epoch publication", param="schema_epochs")
class test_hook_publish01(wttest.WiredTigerTestCase):
    """Check which schema operations the hook publishes and how it assigns epochs."""

    create_config = "key_format=S,value_format=S"
    scenarios = make_scenarios(
        [
            ("table", dict(uri="table:test_hook_publish01")),
            ("explicit", dict(uri="layered:test_hook_publish01")),
        ]
    )

    def stable_epoch(self):
        """Return the connection's stable schema epoch."""
        return int(
            self.conn.query_timestamp("get=stable_disaggregated_schema_epoch"), 16
        )

    def publish_count(self):
        """Return the number of successful publication calls."""
        return self.get_stat(stat.conn.session_table_publish_success)

    def test_create_publishes(self):
        """Creating a new table publishes its schema and advances the epoch."""
        expected_epoch = self.stable_epoch() + 1
        expected_publications = self.publish_count() + 1

        self.session.create(self.uri, self.create_config)

        self.assertEqual(self.stable_epoch(), expected_epoch)
        self.assertEqual(self.publish_count(), expected_publications)

    def test_existing_create_does_not_publish(self):
        """Creating an existing table succeeds without another publication."""
        self.session.create(self.uri, self.create_config)
        expected_publications = self.publish_count()

        self.session.create(self.uri, self.create_config)

        self.assertEqual(self.publish_count(), expected_publications)

    def test_failed_create_does_not_publish(self):
        """An exclusive create of an existing table fails without publishing."""
        self.session.create(self.uri, self.create_config)
        expected_publications = self.publish_count()

        with self.assertRaises(wiredtiger.WiredTigerError):
            self.session.create(self.uri, self.create_config + ",exclusive=true")

        self.assertEqual(self.publish_count(), expected_publications)

    def test_drop_publishes(self):
        """Dropping the table publishes its removal after its earlier create."""
        self.session.create(self.uri, self.create_config)
        expected_epoch = self.stable_epoch() + 1
        expected_publications = self.publish_count() + 1

        self.session.drop(self.uri)

        self.assertEqual(self.stable_epoch(), expected_epoch)
        self.assertEqual(self.publish_count(), expected_publications)

    def test_failed_drop_does_not_publish(self):
        """Dropping a table with an open cursor fails without publishing."""
        self.session.create(self.uri, self.create_config)
        expected_publications = self.publish_count()

        with wttest.open_cursor(self.session, self.uri):
            self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri)))

        self.assertEqual(self.publish_count(), expected_publications)

    def test_unrelated_object_does_not_publish(self):
        """Creating or dropping a same-name file must not publish the table."""
        self.session.create(self.uri, self.create_config)
        expected_publications = self.publish_count()

        local_uri = "file:test_hook_publish01"
        self.session.create(local_uri, self.create_config)
        self.session.drop(local_uri)

        self.assertEqual(self.publish_count(), expected_publications)

    def test_reopen_resumes_epoch(self):
        """Reopening a connection resumes from the last epoch."""
        if self.getDisaggParameters().role != "leader":
            self.skipTest("requires a leader to persist the schema epoch")

        self.session.create(self.uri, self.create_config)
        self.session.checkpoint()
        expected_epoch = self.stable_epoch()

        self.reopen_conn()
        self.assertEqual(self.stable_epoch(), expected_epoch)
