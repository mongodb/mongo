// Does a dbhash check between the donor and recipient primaries during a tenant migration.
'use strict';

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
load("jstests/replsets/libs/tenant_migration_util.js");

const excludedDBs = ["testTenantMigration"];
const testDBName = "testTenantMigration";
const dbhashCollName = "dbhashCheck";
const tenantId = TestData.tenantId;
const migrationId = UUID(TestData.migrationIdString);

const donorRst = new ReplSetTest(TestData.donorConnectionString);
const recipientRst = new ReplSetTest(TestData.recipientConnectionString);

const donorPrimary = donorRst.getPrimary();
const donorPrimaryDB = donorPrimary.getDB(testDBName);

// We assume every db is under the tenant being migrated.
TenantMigrationUtil.checkTenantDBHashes(donorRst, recipientRst, tenantId, excludedDBs);

// Mark that we have completed the dbhash check.
assert.commandWorked(
    donorPrimaryDB[dbhashCollName].insert([{_id: migrationId}], {writeConcern: {w: "majority"}}));
})();
