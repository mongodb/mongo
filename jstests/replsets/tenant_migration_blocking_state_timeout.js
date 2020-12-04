/**
 * Tests tenant migration timeout scenarios.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft, requires_majority_read_concern]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantIdPrefix = "testTenantId";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

function testTimeoutBlockingState() {
    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();
    let savedTimeoutParam = assert.commandWorked(donorPrimary.adminCommand({
        getParameter: 1,
        tenantMigrationBlockingStateTimeoutMS: 1
    }))['tenantMigrationBlockingStateTimeoutMS'];

    assert.commandWorked(
        donorPrimary.adminCommand({setParameter: 1, tenantMigrationBlockingStateTimeoutMS: 5000}));

    const tenantId = `${kTenantIdPrefix}-blockingTimeout`;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    // Fail point to pause right before entering the blocking mode.
    let afterDataSyncFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterDataSync");

    // Run the migration in its own thread, since the initial 'donorStartMigration' command will
    // hang due to the fail point.
    let migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();

    afterDataSyncFp.wait();
    // Fail point to pause the '_sendRecipientSyncDataCommand()' call inside the blocking state
    // until the cancellation token for the method is cancelled.
    let inCallFp =
        configureFailPoint(donorPrimary, "pauseScheduleCallWithCancelTokenUntilCanceled");
    afterDataSyncFp.off();

    tenantMigrationTest.waitForNodesToReachState(
        donorRst.nodes, migrationId, tenantId, TenantMigrationTest.State.kAborted);

    const stateRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.ExceededTimeLimit);

    // This fail point is pausing all calls to recipient, so it has to be disabled to make
    // the 'forget migration' command to work.
    inCallFp.off();
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    assert.commandWorked(donorPrimary.adminCommand(
        {setParameter: 1, tenantMigrationBlockingStateTimeoutMS: savedTimeoutParam}));
}

jsTest.log("Test timeout of the blocking state");
testTimeoutBlockingState();

tenantMigrationTest.stop();
}());
