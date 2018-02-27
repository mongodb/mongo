"""
Testing hook for verifying that the primary has not stepped down or changed.
"""

from __future__ import absolute_import

from . import interface
from ..fixtures import replicaset
from ... import errors

import pymongo.errors


class CheckPrimary(interface.Hook):
    def __init__(self, hook_logger, rs_fixture):
        description = "Verify that the primary has not stepped down or changed"
        interface.Hook.__init__(self, hook_logger, rs_fixture, description)

        if not isinstance(rs_fixture, replicaset.ReplicaSetFixture):
            raise TypeError("{} is not a replica set".format(rs_fixture.__class__.__name__))

        self._rs_fixture = rs_fixture
        self._primary_url = None

    def _get_primary_url(self):
        no_primary_err = errors.ServerFailure("No primary found")

        for node in self._rs_fixture.nodes:
            try:
                is_master = node.mongo_client().admin.command("isMaster")["ismaster"]
            except pymongo.errors.AutoReconnect:
                raise no_primary_err

            if is_master:
                return node.get_driver_connection_url()

        raise no_primary_err

    def before_test(self, test, test_report):
        self._primary_url = self._get_primary_url()

    def after_test(self, test, test_report):
        new_primary_url = self._get_primary_url()

        if new_primary_url != self._primary_url:
            raise errors.ServerFailure("Primary changed, was {} and is now {}".format(
                self._primary_url, new_primary_url))
