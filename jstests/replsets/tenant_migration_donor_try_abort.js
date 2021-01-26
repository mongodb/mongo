/**
 * Tests the donorAbortMigration command during a tenant migration.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";
const kDelayMS =
    500000;  // Using some arbitrarily large delay time in to make sure that the donor is not
             // waiting this long when it receives a donorAbortMigration command.

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

(() => {
    jsTestLog(
        "Test sending donorAbortMigration during a tenant migration while recipientSyncData command repeatedly fails.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    let fp = configureFailPoint(recipientPrimary, "failCommand", {
        failInternalCommands: true,
        errorCode: ErrorCodes.ShutdownInProgress,
        failCommands: ["recipientSyncData"],
    });

    const tenantId = kTenantId;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fp.wait();

    assert.commandWorked(tenantMigrationTest.tryAbortMigration(
        {migrationIdString: migrationOpts.migrationIdString}));

    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    fp.off();
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog(
        "Test sending donorAbortMigration during a tenant migration while waiting for the response of recipientSyncData.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    configureFailPoint(recipientPrimary, "failCommand", {
        failInternalCommands: true,
        blockConnection: true,
        blockTimeMS: kDelayMS,
        failCommands: ["recipientSyncData"],
    });

    const tenantId = kTenantId;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(recipientPrimary.adminCommand({
            currentOp: true,
            $or: [
                {"command.$truncated": {$exists: true}},
                {"command.recipientSyncData": {$exists: true}}
            ]
        }));

        for (let op of res.inprog) {
            if (op.command.recipientSyncData) {
                return true;
            } else {
                if (op.command.$truncated.includes("recipientSyncData")) {
                    return true;
                }
            }
        }
        return false;
    });

    assert.commandWorked(tenantMigrationTest.tryAbortMigration(
        {migrationIdString: migrationOpts.migrationIdString}));

    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while in data sync.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fp.wait();

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      migrationOpts,
                                      donorRstArgs,
                                      TenantMigrationUtil.runTenantMigrationCommand);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(donorPrimary.adminCommand(
            {currentOp: true, desc: "tenant donor migration", tenantId: tenantId}));
        return res.inprog[0].receivedCancelation;
    });

    fp.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while in blocking.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fp.wait();

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      migrationOpts,
                                      donorRstArgs,
                                      TenantMigrationUtil.runTenantMigrationCommand);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(donorPrimary.adminCommand(
            {currentOp: true, desc: "tenant donor migration", tenantId: tenantId}));
        return res.inprog[0].receivedCancelation;
    });

    fp.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration after abort decision.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    fp.off();

    assert.commandWorked(tenantMigrationTest.tryAbortMigration(
        {migrationIdString: migrationOpts.migrationIdString}));

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration after commit decision.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString()
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

    assert.commandFailedWithCode(
        tenantMigrationTest.tryAbortMigration({migrationIdString: migrationOpts.migrationIdString}),
        ErrorCodes.TenantMigrationCommitted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration for a non-existent tenant migration.");

    const donorRst = new ReplSetTest(
        {nodes: 2, name: "donorRst", nodeOptions: Object.assign(migrationX509Options.donor)});

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationIdOther = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString()
    };
    const migrationOptsOther = {
        migrationIdString: migrationIdOther,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString()
    };

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    assert.commandFailedWithCode(tenantMigrationTest.tryAbortMigration(migrationOptsOther),
                                 ErrorCodes.NoSuchTenantMigration);

    // Ensure that the noop is majority committed when we get NoSuchTenantMigration when running
    // donorAbortMigration. In this case, since the donorRst in this test only has two nodes, the
    // majority will include both nodes, so we assert that all the nodes in this replica set have
    // written the noop.
    tenantMigrationTest.getDonorRst().nodes.forEach((node) => {
        const oplog = node.getDB("local").oplog.rs;
        let findRes = oplog.findOne({op: "n", "o.msg": "NoSuchTenantMigration error"});
        assert(findRes);
    });

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();
})();
