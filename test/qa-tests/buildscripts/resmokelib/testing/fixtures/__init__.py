"""
Fixtures for executing JSTests against.
"""

from __future__ import absolute_import

from .interface import Fixture, ReplFixture
from .standalone import MongoDFixture
from .replicaset import ReplicaSetFixture
from .masterslave import MasterSlaveFixture
from .shardedcluster import ShardedClusterFixture


NOOP_FIXTURE_CLASS = "Fixture"

_FIXTURES = {
    "Fixture": Fixture,
    "MongoDFixture": MongoDFixture,
    "ReplicaSetFixture": ReplicaSetFixture,
    "MasterSlaveFixture": MasterSlaveFixture,
    "ShardedClusterFixture": ShardedClusterFixture,
}


def make_fixture(class_name, *args, **kwargs):
    """
    Factory function for creating Fixture instances.
    """

    if class_name not in _FIXTURES:
        raise ValueError("Unknown fixture class '%s'" % (class_name))
    return _FIXTURES[class_name](*args, **kwargs)
