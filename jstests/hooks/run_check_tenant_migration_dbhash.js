// Does a dbhash check between the donor and recipient primaries during a tenant migration.
'use strict';

(function() {

load("jstests/replsets/libs/tenant_migration_util.js");

const excludedDBs = ["testTenantMigration"];
const testDBName = "testTenantMigration";
const dbhashCollName = "dbhashCheck";
const tenantId = TestData.tenantId;
const migrationId = UUID(TestData.migrationIdString);

let donorRst;
let recipientRst;
let donorDB;
while (true) {
    try {
        donorRst = new ReplSetTest(TestData.donorConnectionString);
        recipientRst = new ReplSetTest(TestData.recipientConnectionString);

        // In failover suites, we want to allow retryable writes in case there is a failover while
        // running the tenant dbhash check. In non-failover suites we dont expect to see any
        // failovers, but we run in a session to keep the code simple.
        donorDB =
            new Mongo(donorRst.getURL()).startSession({retryWrites: true}).getDatabase(testDBName);
        break;
    } catch (e) {
        if (!TenantMigrationUtil.checkIfRetriableErrorForTenantDbHashCheck(e)) {
            throw e;
        }
        print(`Got error: ${tojson(e)}. Retrying ReplSetTest setup on retriable error.`);
    }
}

// We assume every db is under the tenant being migrated.
TenantMigrationUtil.checkTenantDBHashes(donorRst, recipientRst, tenantId, excludedDBs);

// Mark that we have completed the dbhash check.
assert.commandWorked(donorDB.runCommand(
    {insert: dbhashCollName, documents: [{_id: migrationId}], writeConcern: {w: "majority"}}));
})();
