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

from contextlib import closing, contextmanager
from pathlib import Path

import compatibility_test
import wiredtiger
from compatibility_version import WTVersion
from wtscenario import make_scenarios


class test_schema_epoch(compatibility_test.CompatibilityTestCase):
    """
    Upgrade and downgrade a disaggregated database between releases using schema epochs.

    Each node takes over from the last checkpoint, checks every earlier table, adds one and
    hands off. Nodes using epochs set the schema epoch to the stable timestamp, as MongoDB
    does. Newer nodes always use epochs, older nodes only when the scenario says so.

    The database starts on the older branch, because it cannot open one created on the newer
    branch.
    """

    older_version_epochs = [
        ("older_without_epochs", dict(older_use_epochs=False)),
        ("older_with_epochs", dict(older_use_epochs=True)),
    ]
    scenarios = make_scenarios(older_version_epochs)

    def setUp(self):
        if self.older_branch < WTVersion("mongodb-9.0"):
            self.skipTest(f"{self.older_branch.name} does not support disaggregated storage")
        super().setUp()
        self.created_table_uris = []

    @contextmanager
    def open_follower(self, branch_name, home):
        """Open a fresh follower on the shared page log and yield a session."""
        Path("shared").mkdir(exist_ok=True)
        Path(home).mkdir()
        Path(home, "kv_home").symlink_to("../shared")
        build_path = Path(self.branch_build_path(branch_name))
        palite_path = build_path / "ext/page_log/palite/libwiredtiger_palite.so"
        config = (
            f'create,extensions=["{palite_path}"],'
            "disaggregated=(page_log=palite,lose_all_my_data=true,role=follower)"
        )
        with (
            closing(wiredtiger.wiredtiger_open(home, config)) as connection,
            closing(connection.open_session()) as session,
        ):
            yield session

    def set_stable(self, session, timestamp, use_epochs):
        """Set the stable timestamp and, with epochs on, the schema epoch to match it."""
        session.connection.set_timestamp(f"stable_timestamp={timestamp:x}")
        if use_epochs:
            session.connection.set_timestamp(f"stable_disaggregated_schema_epoch={timestamp:x}")

    def pick_up_last_checkpoint(self, session):
        """Pick up the last checkpoint in the shared page log and return its timestamp."""
        page_log = session.connection.get_page_log("palite")
        _, _, timestamp, metadata = page_log.pl_get_complete_checkpoint(session)
        page_log.terminate(session)
        session.connection.reconfigure(f'disaggregated=(checkpoint_meta="{metadata}")')
        return timestamp

    def expected_rows(self, uri):
        """Return the rows a table should hold, with values unique to that table."""
        return {f"key{i}": f"{uri}:value{i}" for i in range(100)}

    def create_populated_table(self, session, uri, commit_timestamp):
        """Create a table and insert its expected rows at the commit timestamp."""
        session.create(uri, "key_format=S,value_format=S")
        with closing(session.open_cursor(uri)) as cursor:
            session.begin_transaction()
            for key, value in self.expected_rows(uri).items():
                cursor[key] = value
            session.commit_transaction(f"commit_timestamp={commit_timestamp:x}")

    def verify_rows(self, session, uris):
        """Assert that each table holds exactly its expected rows."""
        for uri in uris:
            with closing(session.open_cursor(uri)) as cursor:
                self.assertEqual(dict(cursor), self.expected_rows(uri), uri)

    def run_leader(self, branch_name, home, commit_timestamp, use_epochs):
        """Take over from the last checkpoint, check earlier tables, add one and hand off."""
        table_uri = f"layered:{home}"
        with self.open_follower(branch_name, home) as session:
            # The first node has no checkpoint to pick up, so default to a value of one.
            timestamp = self.pick_up_last_checkpoint(session) if self.created_table_uris else 1
            self.set_stable(session, timestamp, use_epochs)
            session.connection.reconfigure("disaggregated=(role=leader)")

            self.verify_rows(session, self.created_table_uris)

            self.create_populated_table(session, table_uri, commit_timestamp)
            if use_epochs:
                session.publish(table_uri, f"disaggregated=(schema_epoch={commit_timestamp:x})")
            self.set_stable(session, commit_timestamp, use_epochs)

            session.checkpoint()
            session.connection.reconfigure("disaggregated=(role=follower)")

    def run_leader_on(self, branch, home, commit_timestamp):
        """Run a leader with the branch's build, then record the table it added."""
        use_epochs = branch == self.newer_branch or self.older_use_epochs
        self.run_method_on_branch(
            branch,
            f"run_leader(branch_name={branch.name!r}, home={home!r}, "
            f"commit_timestamp={commit_timestamp}, use_epochs={use_epochs})",
        )
        self.created_table_uris.append(f"layered:{home}")

    def test_schema_epoch_compatibility(self):
        """Upgrade, downgrade and re-upgrade, checking each node reads earlier tables."""
        self.run_leader_on(self.older_branch, home="legacy", commit_timestamp=10)
        self.run_leader_on(self.newer_branch, home="upgrade", commit_timestamp=20)
        self.run_leader_on(self.older_branch, home="downgrade", commit_timestamp=30)
        self.run_leader_on(self.newer_branch, home="reupgrade", commit_timestamp=40)


if __name__ == "__main__":
    compatibility_test.run()
