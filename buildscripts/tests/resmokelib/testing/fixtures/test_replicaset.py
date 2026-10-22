"""Unittest for the resmokelib.testing.fixtures.replicaset module."""

import unittest
from unittest import mock

import bson

from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib

UNINITIALIZED_OPTIME = {"ts": bson.Timestamp(0, 0), "t": -1}
SELF_OPTIME = {"ts": bson.Timestamp(1791469014, 25), "t": 12}


def _make_client(members):
    client = mock.MagicMock()
    client.address = ("localhost", 20027)
    client.admin.command.return_value = {"members": members}
    return client


class TestGetLastOptime(unittest.TestCase):
    def setUp(self) -> None:
        self.fixturelib = FixtureLib()

    def test_self_only_on_own_member(self):
        members = [
            {"_id": 0, "name": "localhost:20026", "optime": UNINITIALIZED_OPTIME},
            {"_id": 1, "name": "localhost:20027", "optime": SELF_OPTIME, "self": True},
        ]
        optime = replicaset.get_last_optime(_make_client(members), self.fixturelib)
        self.assertEqual(optime, SELF_OPTIME)

    def test_self_reported_on_every_member(self):
        # Disaggregated storage reports 'self' on every member, set to False for other members.
        members = [
            {
                "_id": 0,
                "name": "localhost:20026",
                "optime": UNINITIALIZED_OPTIME,
                "self": False,
            },
            {"_id": 1, "name": "localhost:20027", "optime": SELF_OPTIME, "self": True},
        ]
        optime = replicaset.get_last_optime(_make_client(members), self.fixturelib)
        self.assertEqual(optime, SELF_OPTIME)

    def test_uninitialized_self_optime_raises(self):
        members = [
            {"_id": 0, "name": "localhost:20026", "optime": SELF_OPTIME, "self": False},
            {
                "_id": 1,
                "name": "localhost:20027",
                "optime": UNINITIALIZED_OPTIME,
                "self": True,
            },
        ]
        with self.assertRaises(self.fixturelib.ServerFailure):
            replicaset.get_last_optime(_make_client(members), self.fixturelib)


if __name__ == "__main__":
    unittest.main()
