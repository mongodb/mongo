/**
 * Tests that tenant migration fails with NamespaceExists if the recipient already has tenant's
 * data.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
        setParameter: {
            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    })
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kTenantId = "testTenantId";
const kNs = kTenantId + "_testDb.testColl";

assert.commandWorked(tenantMigrationTest.getDonorPrimary().getCollection(kNs).insert({_id: 0}));

jsTest.log("Start a tenant migration and verify that it commits successfully");

(() => {
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(
        tenantMigrationTest.getDonorRst().nodes, migrationId, kTenantId);
})();

jsTest.log(
    "Retry the migration without dropping tenant database on the recipient and verify that " +
    "the migration aborted with NamespaceExists as the abort reason");

(() => {
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.NamespaceExists);
})();

donorRst.stopSet();
tenantMigrationTest.stop();
})();
