/**
 * Tests the donorAbortMigration command during a tenant migration.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";
const kDelayMS =
    500000;  // Using some arbitrarily large delay time in to make sure that the donor is not
             // waiting this long when it receives a donorAbortMigration command.

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

(() => {
    jsTestLog("Test sending donorAbortMigration before an instance's future chain begins.");

    const tmt = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tmt.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeEnteringFutureChain");

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tmt.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tmt.getDonorRst());

    const startMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    startMigrationThread.start();

    fp.wait();

    const tryAbortThread =
        new Thread(TenantMigrationUtil.tryAbortMigrationAsync, migrationOpts, donorRstArgs);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        const op = res.inprog.find(op => extractUUIDFromObject(op.instanceID) === migrationId);
        return op.receivedCancellation;
    });

    fp.off();

    startMigrationThread.join();
    tryAbortThread.join();
    let r = assert.commandWorked(tryAbortThread.returnData());

    TenantMigrationTest.assertAborted(tmt.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tmt.stop();
})();

(() => {
    jsTestLog(
        "Test sending donorAbortMigration during a tenant migration while recipientSyncData " +
        "command repeatedly fails with retryable errors.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    if (TenantMigrationUtil.isShardMergeEnabled(
            tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping test, Shard Merge does not support retry");
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

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    fp.off();
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while find command " +
              "against admin.system.keys repeatedly fails with retryable errors.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    if (TenantMigrationUtil.isShardMergeEnabled(
            tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping test, Shard Merge does not support retry");
        return;
    }

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    let fp = configureFailPoint(recipientPrimary, "failCommand", {
        failInternalCommands: true,
        errorCode: ErrorCodes.ShutdownInProgress,
        failCommands: ["find"],
        namespace: "admin.system.keys"
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

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    fp.off();
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while waiting for the " +
              "response of recipientSyncData.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

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

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while waiting for the " +
              "response of find against admin.system.keys.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    configureFailPoint(recipientPrimary, "failCommand", {
        failInternalCommands: true,
        blockConnection: true,
        blockTimeMS: kDelayMS,
        failCommands: ["find"],
        namespace: "admin.system.keys"
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
        const res = assert.commandWorked(recipientPrimary.adminCommand(
            {currentOp: true, $and: [{"command.find": "system.keys"}, {"command.$db": "admin"}]}));
        return res.inprog.length == 1;
    });
    assert.commandWorked(tenantMigrationTest.tryAbortMigration(
        {migrationIdString: migrationOpts.migrationIdString}));
    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration before fetching keys from admin.system.keys.");

    const tmt = new TenantMigrationTest({name: jsTestName()});

    const barrierBeforeFetchingKeys =
        configureFailPoint(tmt.getDonorPrimary(), "pauseTenantMigrationBeforeFetchingKeys");

    configureFailPoint(tmt.getRecipientPrimary(), "failCommand", {
        failInternalCommands: true,
        blockConnection: true,
        blockTimeMS: kDelayMS,
        failCommands: ["find"],
        namespace: "admin.system.keys"
    });

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tmt.getRecipientConnString(),
    };
    assert.commandWorked(tmt.startMigration(migrationOpts));

    barrierBeforeFetchingKeys.wait();

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tmt.getDonorRst());
    const tryAbortThread =
        new Thread(TenantMigrationUtil.tryAbortMigrationAsync, migrationOpts, donorRstArgs);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            tmt.getDonorPrimary().adminCommand({currentOp: true, desc: "tenant donor migration"}));
        const op = res.inprog.find(op => extractUUIDFromObject(op.instanceID) === migrationId);
        return op.receivedCancellation;
    });
    barrierBeforeFetchingKeys.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    TenantMigrationTest.assertAborted(tmt.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tmt.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration to interrupt waiting for keys to replicate.");

    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        settings: {chainingAllowed: false},
        nodeOptions: migrationX509Options.donor
    });

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});

    const tenantId = kTenantId;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };

    // Stop replicating on one of the secondaries so the donor cannot satisfy write concerns that
    // require all nodes but can still commit majority writes.
    const delayedSecondary = donorRst.getSecondaries()[1];
    stopServerReplication(delayedSecondary);

    const barrierBeforeWaitingForKeyWC = configureFailPoint(
        donorRst.getPrimary(), "pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate");

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait for the donor to begin waiting for replication of the copied keys.
    barrierBeforeWaitingForKeyWC.wait();
    barrierBeforeWaitingForKeyWC.off();
    sleep(500);

    // The migration should be unable to progress past the aborting index builds state because
    // it cannot replicate the copied keys to every donor node.
    let res = assert.commandWorked(tenantMigrationTest.runDonorStartMigration(migrationOpts));
    assert.eq("aborting index builds", res.state, tojson(res));

    // Abort the migration and the donor should stop waiting for key replication, despite the write
    // concern still not being satisfied.
    assert.commandWorked(tenantMigrationTest.tryAbortMigration(migrationOpts));

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */));

    restartServerReplication(delayedSecondary);

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while in data sync.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

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
    const tryAbortThread =
        new Thread(TenantMigrationUtil.tryAbortMigrationAsync, migrationOpts, donorRstArgs);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        const op = res.inprog.find(op => extractUUIDFromObject(op.instanceID) === migrationId);
        return op.receivedCancellation;
    });

    fp.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration while in blocking.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

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
    const tryAbortThread =
        new Thread(TenantMigrationUtil.tryAbortMigrationAsync, migrationOpts, donorRstArgs);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        const op = res.inprog.find(op => extractUUIDFromObject(op.instanceID) === migrationId);
        return op.receivedCancellation;
    });

    fp.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                      ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration after abort decision.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(migrationOpts),
                                      ErrorCodes.InternalError);

    fp.off();

    assert.commandWorked(tenantMigrationTest.tryAbortMigration(
        {migrationIdString: migrationOpts.migrationIdString}));

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration during a tenant migration after commit decision.");

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const tenantId = kTenantId;
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString()
    };

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    assert.commandFailedWithCode(
        tenantMigrationTest.tryAbortMigration({migrationIdString: migrationOpts.migrationIdString}),
        ErrorCodes.TenantMigrationCommitted);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test sending donorAbortMigration for a non-existent tenant migration.");

    const donorRst =
        new ReplSetTest({nodes: 2, name: "donorRst", nodeOptions: migrationX509Options.donor});

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});

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

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();
})();
