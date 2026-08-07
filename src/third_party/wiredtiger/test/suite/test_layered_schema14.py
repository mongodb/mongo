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

# Checkpoint pickup must not adopt a leader-assigned btree ID that a stable file in the
# follower's local metadata already uses. The disaggregated block manager addresses every
# page by that ID, so the second handle would read the first table's blocks and history
# store entries. The follower cannot renumber the incoming table (the ID is the leader's
# key into shared storage) and cannot drop the table it collides with, and no retry can
# resolve it, so pickup panics.
#
# Each conflict scenario injects a decoy stable file entry into the follower's local metadata
# carrying the ID the next pickup is about to bring in, covering both paths that adopt an
# incoming ID: a table that is entirely new to the follower, and a table the follower
# already published locally and is only missing the stable constituent of.

import re
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema14(wttest.WiredTigerTestCase, suite_subprocess, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'
    # The restart scenario needs local metadata to survive, so it cannot skip file system syncs.
    conn_config_follower_durable = conn_base_config + 'disaggregated=(role="follower")'

    uri = f'layered:{test_name}'
    uri2 = f'layered:{test_name}_2'
    uri3 = f'layered:{test_name}_3'
    decoy_uri = f'file:{test_name}_decoy.wt_stable'

    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def panic_regex(self, first_uri, second_uri):
        """
        The panic must report the shared ID and name both conflicting files. Equal IDs sort
        in an unspecified order, so accept either arrangement of the pair.
        """
        first = re.escape(first_uri)
        second = re.escape(second_uri)
        return (f'/checkpoint pickup would leave btree ID \\d+ shared by '
                f'("{first}" and "{second}"|"{second}" and "{first}") '
                f'in the local metadata/')

    def recovery_conflict_regex(self, first_uri, second_uri):
        """
        Recovery indexes files by ID and refuses to open metadata holding two on one ID, so a
        node cannot come back up while the conflict is still there.
        """
        first = re.escape(first_uri)
        second = re.escape(second_uri)
        return (f'/metadata corruption: files ({first} and {second}|{second} and {first}) '
                f'have the same file ID \\d+/')


    # Subprocess bodies for the panic tests below. These must not be named test_*: the runner
    # would collect them and the panic would abort the test process itself.
    def subprocess_new_table_id_conflict_panics(self):
        """Subprocess body for the new-table conflict test; expected to panic/abort."""
        self.session.create(self.uri, self.table_config)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()

        # The leader creates a second table that the follower has never seen.
        self.session.create(self.uri2, self.table_config)
        self.leader_checkpoint(20)
        self.assertFalse(self.uri_in_local_metadata(conn_follow, self.uri2))

        # Give the follower a stable file already holding the incoming table's ID.
        self.inject_stable_entry(conn_follow, self.decoy_uri,
            self.stable_config(self.conn, self.uri2))

        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.

    def subprocess_published_table_id_conflict_panics(self):
        """Subprocess body for the published-table conflict test; expected to panic/abort."""
        self.set_stable_epoch(1)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 10)
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

        conn_follow, session_follow = self.open_follower()
        self.set_stable_epoch(1, conn_follow)

        # Both nodes create a second table through the publish API. Only the leader creates
        # its stable constituent, so the follower adopts the leader's ID at pickup.
        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 20)
        session_follow.create(self.uri2, self.table_config)
        self.publish(self.uri2, 20, session_follow)
        self.set_stable_epoch(20)
        self.leader_checkpoint(2)
        self.assertFalse(self.uri_stable_exists(conn_follow, self.uri2))

        # Give the follower a stable file already holding the incoming table's ID.
        self.inject_stable_entry(conn_follow, self.decoy_uri,
            self.stable_config(self.conn, self.uri2))

        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.

    def subprocess_restart_after_conflict_panics(self):
        """Subprocess body for the restart test; expected to panic/abort."""
        self.session.create(self.uri, self.table_config)
        self.leader_checkpoint(10)

        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower_durable)
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')

        # Duplicate an ID the follower already holds, then shut down cleanly so the
        # conflicting pair is what the node comes back up with.
        self.inject_stable_entry(conn_follow, self.decoy_uri,
            self.stable_config(conn_follow, self.uri))
        session_follow.close()
        conn_follow.close()

        # A pickup is only attempted for a checkpoint the node has not already applied.
        self.leader_checkpoint(20)

        # Recovery reads the local metadata before any pickup runs, and stops here.
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower_durable)
        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.


    # Positive test: an ordinary pickup assigns distinct IDs and does not panic.
    def test_pickup_unique_ids(self):
        """Picking up tables whose IDs are unused locally leaves the follower readable."""
        self.session.create(self.uri, self.table_config)
        self.session.create(self.uri2, self.table_config)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()

        # A table created after the follower opened arrives through a later pickup.
        self.session.create(self.uri3, self.table_config)
        cursor = self.session.open_cursor(self.uri3)
        self.session.begin_transaction()
        cursor[1] = 'aaa'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))
        cursor.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_stable_exists(conn_follow, self.uri3))
        cursor = session_follow.open_cursor(self.uri3)
        self.assertEqual(cursor[1], 'aaa')
        cursor.close()

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')


    # Negative tests: an ID the local metadata already uses panics. Each scenario runs in a
    # subprocess because the panic aborts the process.
    def test_new_table_id_conflict_panics(self):
        """
        A layered table that is new to the follower must not be picked up when its btree ID is
        already used by a local stable file; the merge fails and unrolls instead.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.leader_checkpoint(1)
        self.run_panic_subprocess('new_table_id_conflict_panics',
            self.panic_regex(self.decoy_uri, self.stable_uri(self.uri2)))

    def test_published_table_id_conflict_panics(self):
        """
        A locally published table whose stable constituent arrives through pickup must not adopt
        a btree ID already used by a local stable file; the merge fails and unrolls instead.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.leader_checkpoint(1)
        self.run_panic_subprocess('published_table_id_conflict_panics',
            self.panic_regex(self.decoy_uri, self.stable_uri(self.uri2)))

    def test_restart_after_conflict_panics(self):
        """
        A node that halts on a conflict cannot come back up while the conflict is still in its
        local metadata: recovery indexes files by ID and refuses the duplicate.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.leader_checkpoint(1)
        self.run_panic_subprocess('restart_after_conflict_panics',
            self.recovery_conflict_regex(self.decoy_uri, self.stable_uri(self.uri)))

