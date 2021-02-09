/**
 * Tests that restarting a migration attempt after a failover fails if the donor and recipient no
 * longer share the same FCV.
 * @tags: [requires_majority_read_concern, incompatible_with_windows_tls]
 */

(function() {
"use strict";

function runTest(downgradeFCV) {
    load("jstests/libs/fail_point_util.js");
    load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
    load("jstests/libs/parallelTester.js");  // for 'Thread'
    load("jstests/replsets/libs/tenant_migration_test.js");
    load("jstests/replsets/libs/tenant_migration_util.js");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    tenantMigrationTest.insertDonorDB(dbName, collName);

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };

    // Configure a failpoint to have the recipient primary hang after a successful initial
    // comparison.
    const recipientDB = recipientPrimary.getDB(dbName);
    const hangAfterFirstFCVcheck =
        configureFailPoint(recipientDB, "fpAfterComparingRecipientAndDonorFCV", {action: "hang"});

    // Start a migration and wait for recipient to hang at the failpoint.
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    hangAfterFirstFCVcheck.wait();

    // Downgrade the FCV for the donor set.
    assert.commandWorked(donorPrimary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Step up a new node in the recipient set and trigger a failover. The new primary should
    // attempt to resume cloning, but fail upon re-checking the FCVs.
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const newRecipientPrimary = recipientRst.getSecondaries()[0];
    recipientRst.awaitLastOpCommitted();
    assert.commandWorked(newRecipientPrimary.adminCommand({replSetStepUp: 1}));
    hangAfterFirstFCVcheck.off();
    recipientRst.getPrimary();

    // Make sure we see the FCV mismatch detection message on the recipient regardless.
    checkLog.containsJson(newRecipientPrimary, 5382300);

    // Upgrade again to check on the status of the migration from the donor's point of view.
    assert.commandWorked(donorPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    tenantMigrationTest.stop();
}

runFeatureFlagMultiversionTest('featureFlagTenantMigrations', runTest);
})();
