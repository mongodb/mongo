// Does a dbhash check between the donor and recipient primaries during a tenant migration.
'use strict';

(function() {

load("jstests/replsets/libs/tenant_migration_util.js");

const excludedDBs = ["testTenantMigration"];
const testDBName = "testTenantMigration";
const dbhashCollName = "dbhashCheck";
const localDBName = "local";
const tenantId = TestData.tenantId;
const migrationId = UUID(TestData.migrationIdString);

let donorRst;
let recipientRst;
let donorDB;
// For shard merge we need to use the local DB that is not blocked by tenant access blockers.
let primaryLocalDB;
while (true) {
    try {
        donorRst = new ReplSetTest(TestData.donorConnectionString);
        recipientRst = new ReplSetTest(TestData.recipientConnectionString);

        // In failover suites, we want to allow retryable writes in case there is a failover while
        // running the tenant dbhash check. In non-failover suites we dont expect to see any
        // failovers, but we run in a session to keep the code simple.
        donorDB =
            new Mongo(donorRst.getURL()).startSession({retryWrites: true}).getDatabase(testDBName);
        primaryLocalDB =
            new Mongo(donorRst.getURL()).startSession({retryWrites: true}).getDatabase(localDBName);

        break;
    } catch (e) {
        if (!TenantMigrationUtil.checkIfRetryableErrorForTenantDbHashCheck(e)) {
            throw e;
        }
        print(`Got error: ${tojson(e)}. Retrying ReplSetTest setup on retryable error.`);
    }
}

const skipTempCollections = TestData.skipTempCollections ? true : false;

// We assume every db is under the tenant being migrated.
if (TestData.tenantIds) {
    TestData.tenantIds.forEach(
        tenantId => TenantMigrationUtil.checkTenantDBHashes(
            {donorRst, recipientRst, tenantId, excludedDBs, skipTempCollections}));
} else {
    TenantMigrationUtil.checkTenantDBHashes(
        {donorRst, recipientRst, tenantId, excludedDBs, skipTempCollections});
}

// Mark that we have completed the dbhash check.
// useLocalDBForDbCheck is used for Shard Merge since we use the local DB for validation.
if (TestData.useLocalDBForDBCheck) {
    assert.commandWorked(primaryLocalDB.runCommand(
        {insert: dbhashCollName, documents: [{_id: migrationId}], writeConcern: {w: 1}}));
} else {
    assert.commandWorked(donorDB.runCommand(
        {insert: dbhashCollName, documents: [{_id: migrationId}], writeConcern: {w: "majority"}}));
}
})();
