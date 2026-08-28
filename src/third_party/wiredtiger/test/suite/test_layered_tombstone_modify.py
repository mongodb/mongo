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

# Verify that modify operations preserve values as they enter or leave the
# layered tombstone namespace on stable and ingest tables.


from contextlib import closing
from typing import NamedTuple

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

class ModifyTestCase(NamedTuple):
    """A test case with its input and expected output."""

    base_value: bytes
    modifications: list
    expected_value: bytes

def encode_bson_size(size: int) -> bytes:
    """The first four bytes of a BSON document: the size."""
    return size.to_bytes(4, byteorder="little", signed=True)

def make_bson_like_value(size: int) -> bytes:
    """
    Make a BSON-like value of the given size.

    Structure:
      4 bytes: little-endian document size
      size - 5 bytes: placeholder payload
      1 byte: BSON's required trailing NUL
    """
    return encode_bson_size(size) + b"x" * (size - 5) + b"\x00"


@disagg_test_class
class test_layered_tombstone_modify(wttest.WiredTigerTestCase):
    uri = f"layered:{__qualname__}"

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    modify_cases = (
        # Move into the namespace by shrinking a BSON document to 0x1414 bytes.
        ModifyTestCase(
            base_value=make_bson_like_value(size=0x1415),
            modifications=[
                wiredtiger.Modify(encode_bson_size(0x1414), 0, 4),
                wiredtiger.Modify(b"", 0x1410, 1),
            ],
            expected_value=make_bson_like_value(0x1414),
        ),
        # Move out of the namespace by growing a BSON document to 0x1415 bytes.
        ModifyTestCase(
            base_value=make_bson_like_value(size=0x1414),
            modifications=[
                wiredtiger.Modify(encode_bson_size(0x1415), 0, 4),
                wiredtiger.Modify(b"x", 0x1410, 0),
            ],
            expected_value=make_bson_like_value(0x1415),
        ),
        # Move into the namespace by replacing the whole value with 0x1414.
        ModifyTestCase(
            base_value=b"ab",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 2)],
            expected_value=b"\x14\x14",
        ),
        # Move into the namespace by replacing the whole value with 0x141414.
        ModifyTestCase(
            base_value=b"ab",
            modifications=[wiredtiger.Modify(b"\x14\x14\x14", 0, 2)],
            expected_value=b"\x14\x14\x14",
        ),
        # Move into the namespace by prefixing the value with 0x1414.
        ModifyTestCase(
            base_value=b"ab",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 0)],
            expected_value=b"\x14\x14ab",
        ),
        # Move into the namespace by prefixing the value with 0x1414.
        ModifyTestCase(
            base_value=b"ab\x14",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 0)],
            expected_value=b"\x14\x14ab\x14",
        ),
        # Move into the namespace by replacing the first two bytes with 0x1414.
        ModifyTestCase(
            base_value=b"abcd",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 2)],
            expected_value=b"\x14\x14cd",
        ),
        # Move into the namespace by replacing the whole value with 0x1414.
        ModifyTestCase(
            base_value=b"\x14\x15",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 2)],
            expected_value=b"\x14\x14",
        ),
        # Move into the namespace by replacing the whole value with 0x1414.
        ModifyTestCase(
            base_value=b"\x14\x15ab\x14",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 5)],
            expected_value=b"\x14\x14",
        ),
        # Remain in the namespace by replacing the whole value with 0x1414.
        ModifyTestCase(
            base_value=b"\x14\x14",
            modifications=[wiredtiger.Modify(b"\x14\x14", 0, 2)],
            expected_value=b"\x14\x14",
        ),
        # Remain in the namespace by appending a suffix.
        ModifyTestCase(
            base_value=b"\x14\x14",
            modifications=[wiredtiger.Modify(b"ab", 2, 0)],
            expected_value=b"\x14\x14ab",
        ),
        # Remain in the namespace by appending a suffix that ends in 0x14.
        ModifyTestCase(
            base_value=b"\x14\x14",
            modifications=[wiredtiger.Modify(b"ab\x14", 2, 0)],
            expected_value=b"\x14\x14ab\x14",
        ),
        # Move out of the namespace by replacing the second 0x14 byte.
        ModifyTestCase(
            base_value=b"\x14\x14",
            modifications=[wiredtiger.Modify(b"\x15", 1, 1)],
            expected_value=b"\x14\x15",
        ),
        # Move out of the namespace by inserting bytes between the two 0x14 bytes.
        ModifyTestCase(
            base_value=b"\x14\x14",
            modifications=[wiredtiger.Modify(b"\x15ab", 1, 0)],
            expected_value=b"\x14\x15ab\x14",
        ),
        # Remain in the namespace by removing the whole suffix.
        ModifyTestCase(
            base_value=b"\x14\x14ab",
            modifications=[wiredtiger.Modify(b"", 2, 2)],
            expected_value=b"\x14\x14",
        ),
        # Remain in the namespace by appending a trailing 0x14 byte.
        ModifyTestCase(
            base_value=b"\x14\x14ab",
            modifications=[wiredtiger.Modify(b"\x14", 4, 0)],
            expected_value=b"\x14\x14ab\x14",
        ),
        # Move out of the namespace by replacing the first 0x14 byte.
        ModifyTestCase(
            base_value=b"\x14\x14ab",
            modifications=[wiredtiger.Modify(b"a", 0, 1)],
            expected_value=b"a\x14ab",
        ),
        # Remain in the namespace by removing the whole suffix.
        ModifyTestCase(
            base_value=b"\x14\x14ab\x14",
            modifications=[wiredtiger.Modify(b"", 2, 3)],
            expected_value=b"\x14\x14",
        ),
        # Remain in the namespace by removing the trailing 0x14 byte.
        ModifyTestCase(
            base_value=b"\x14\x14ab\x14",
            modifications=[wiredtiger.Modify(b"", 4, 1)],
            expected_value=b"\x14\x14ab",
        ),
        # Move out of the namespace by replacing the second 0x14 byte.
        ModifyTestCase(
            base_value=b"\x14\x14ab\x14",
            modifications=[wiredtiger.Modify(b"\x15", 1, 1)],
            expected_value=b"\x14\x15ab\x14",
        ),
    )

    def role_conn_config(self, role: str) -> str:
        """Configuration string for a connection in the given role."""
        return (
            f"{self.extensionsConfig()},create,statistics=(all),"
            f'disaggregated=(role="{role}")'
        )

    def conn_config(self) -> str:
        """Configuration string for the leader connection."""
        return self.role_conn_config("leader")

    def setUp(self):
        super().setUp()

        self.ignoreStdoutPattern(
            "stable table value in the tombstone namespace"
        )

        table_config = "key_format=i,value_format=u"
        self.session.create(self.uri, table_config)

        self.follow_conn = self.wiredtiger_open(
            "follower", self.role_conn_config("follower")
        )

        self.follow = self.follow_conn.open_session("")
        self.follow.create(self.uri, table_config)

    def write_value(self, session, key, value, commit_ts=None):
        """Write one key-value pair in its own transaction."""
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session, commit_timestamp=commit_ts):
                cursor[key] = value

    def apply_modify(self, session, key, modifications, commit_ts=None):
        """Perform modify operation(s) on the given key."""
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session, commit_timestamp=commit_ts):
                cursor.set_key(key)
                self.assertEqual(cursor.modify(modifications), 0)

    def remove_value(self, session, key, commit_ts=None):
        """Remove one key in its own transaction."""
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session, commit_timestamp=commit_ts):
                cursor.set_key(key)
                self.assertEqual(cursor.remove(), 0)

    def check_value(self, session, key, expected_value):
        """Assert the value is byte-exact, so a change of encoding cannot slip through."""
        with closing(session.open_cursor(self.uri)) as cursor:
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), expected_value)

    def checkpoint_to_follower(self, stable_ts):
        """Publish the leader's data to the follower's stable table."""
        self.conn.set_timestamp(
            "stable_timestamp=" + self.timestamp_str(stable_ts)
        )
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.follow_conn, self.conn)

    def test_modify_namespace_transitions_leader_stable(self):
        """Modify on the leader, where the values exist only in the stable table."""
        for key, case in enumerate(self.modify_cases, 1):
            self.write_value(self.session, key, case.base_value)
            self.apply_modify(self.session, key, case.modifications)
            self.check_value(self.session, key, case.expected_value)

    def test_modify_namespace_transitions_follower_ingest(self):
        """Modify on the follower, where the values exist only in its ingest table."""
        for key, case in enumerate(self.modify_cases, 1):
            self.write_value(self.follow, key, case.base_value)
            self.apply_modify(self.follow, key, case.modifications)
            self.check_value(self.follow, key, case.expected_value)

    def test_modify_namespace_transitions_follower_stable(self):
        """Read on the follower the values a leader modify produced."""

        # Modify values as the leader.
        for key, case in enumerate(self.modify_cases, 1):
            self.write_value(self.session, key, case.base_value, commit_ts=1)
            self.apply_modify(
                self.session, key, case.modifications, commit_ts=2
            )

        self.checkpoint_to_follower(2)

        # Read the modified values on the follower.
        for key, case in enumerate(self.modify_cases, 1):
            self.check_value(self.follow, key, case.expected_value)

    def test_modify_namespace_transitions_follower_stable_to_ingest(self):
        """Modify stable-only values on the follower, forcing the results into ingest."""

        # Populate values as the leader.
        for key, case in enumerate(self.modify_cases, 1):
            self.write_value(self.session, key, case.base_value, commit_ts=1)

        self.checkpoint_to_follower(1)

        # The follower cannot write to stable, so each modify materializes into ingest.
        for key, case in enumerate(self.modify_cases, 1):
            self.apply_modify(
                self.follow, key, case.modifications, commit_ts=2
            )
            self.check_value(self.follow, key, case.expected_value)

    def test_modify_deleted_key_follower_ingest(self):
        """A modify on a removed key returns WT_NOTFOUND rather than resurrecting the value."""
        key = 1
        self.write_value(self.follow, key, b"value")
        self.remove_value(self.follow, key)

        # A removal stores the reserved tombstone marker in ingest; a modify cannot build on it.
        with closing(self.follow.open_cursor(self.uri)) as cursor:
            with self.transaction(self.follow, rollback=True):
                cursor.set_key(key)
                self.assertEqual(
                    cursor.modify([wiredtiger.Modify(b"x", 0, 1)]),
                    wiredtiger.WT_NOTFOUND,
                )

    def test_modify_namespace_transitions_survive_stepup_drain(self):
        """Step up after follower modifies; the drain moves every ingest version to stable."""
        self.checkpoint_to_follower(1)

        for key, case in enumerate(self.modify_cases, 1):
            self.write_value(self.follow, key, case.base_value, commit_ts=2)
            self.apply_modify(self.follow, key, case.modifications, commit_ts=3)

        self.conn.close("debug=(skip_checkpoint=true)")
        self.follow_conn.reconfigure('disaggregated=(role="leader")')
        self.follow_conn.set_timestamp(
            "stable_timestamp=" + self.timestamp_str(3)
        )
        self.follow.checkpoint()

        for key, case in enumerate(self.modify_cases, 1):
            self.check_value(self.follow, key, case.expected_value)
