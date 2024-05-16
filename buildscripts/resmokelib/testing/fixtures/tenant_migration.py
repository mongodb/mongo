"""Fixture with multiple replica sets for executing JSTests against."""

from buildscripts.resmokelib.testing.fixtures.fixturelib import with_naive_retry
from buildscripts.resmokelib.testing.fixtures import multi_replica_set


class TenantMigrationFixture(multi_replica_set.MultiReplicaSetFixture):
    """Fixture which provides JSTests with a set of replica sets to run tenant migration against."""

    def __init__(self, logger, job_num, fixturelib, **options):
        """Initialize TenantMigrationFixture with different options for the replica set processes."""
        options["common_mongod_options"].update({"serverless": True})
        multi_replica_set.MultiReplicaSetFixture.__init__(
            self, logger, job_num, fixturelib, **options
        )

        # The ReplicaSetFixture for the replica set that starts out owning the data (i.e. the
        # replica set that driver should connect to when running commands).
        self.replica_set_with_tenant = self.replica_sets[0]

    def setup(self):
        """Set up the replica sets."""
        for replica_set in self.replica_sets:
            replica_set.setup()
            self._create_tenant_migration_donor_and_recipient_roles(replica_set)

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that currently starts out owning the data."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return self.replica_set_with_tenant.get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that currently starts out owning the data."""
        if not self.replica_set_with_tenant:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.replica_set_with_tenant.get_driver_connection_url()

    def _create_client(self, fixture, **kwargs):
        return fixture.mongo_client(
            username=self.auth_options["username"],
            password=self.auth_options["password"],
            authSource=self.auth_options["authenticationDatabase"],
            authMechanism=self.auth_options["authenticationMechanism"],
            uuidRepresentation="standard",
            **kwargs,
        )

    def _create_tenant_migration_donor_and_recipient_roles(self, rs):
        """Create a role for tenant migration donor and recipient."""
        primary = rs.get_primary()
        primary_client = self._create_client(primary)

        try:
            with_naive_retry(
                lambda: primary_client.admin.command(
                    {
                        "createRole": "tenantMigrationDonorRole",
                        "privileges": [
                            {"resource": {"cluster": True}, "actions": ["runTenantMigration"]},
                            {
                                "resource": {"db": "admin", "collection": "system.keys"},
                                "actions": ["find"],
                            },
                        ],
                        "roles": [],
                    }
                )
            )
        except:
            self.logger.exception(
                "Error creating tenant migration donor role on primary on port %d of replica"
                + " set '%s'.",
                primary.port,
                rs.replset_name,
            )
            raise

        try:
            with_naive_retry(
                lambda: primary_client.admin.command(
                    {
                        "createRole": "tenantMigrationRecipientRole",
                        "privileges": [
                            {
                                "resource": {"cluster": True},
                                "actions": ["listDatabases", "useUUID", "advanceClusterTime"],
                            },
                            {
                                "resource": {"db": "", "collection": ""},
                                "actions": ["listCollections"],
                            },
                            {
                                "resource": {"anyResource": True},
                                "actions": ["dbStats", "collStats", "find", "listIndexes"],
                            },
                        ],
                        "roles": [],
                    }
                )
            )
        except:
            self.logger.exception(
                "Error creating tenant migration recipient role on primary on port %d of replica"
                + " set '%s'.",
                primary.port,
                rs.replset_name,
            )
            raise
