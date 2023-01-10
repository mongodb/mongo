/**
 * Tests that the donor retries its steps until success, or it gets an error that should lead to
 * an abort decision.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
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
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantIdPrefix = "testTenantId";
let testNum = 0;

function setup() {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 1,
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
            setParameter: {
                // Set the delay before a donor state doc is garbage collected to be short to speed
                // up the test.
                tenantMigrationGarbageCollectionDelayMS: 0,
                ttlMonitorSleepSecs: 1
            }
        })
    });

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        donorRst: donorRst,
        quickGarbageCollection: true,
    });
    return {
        tenantMigrationTest,
        teardown: function() {
            donorRst.stopSet();
            tenantMigrationTest.stop();
        },
    };
}

function makeTenantId() {
    return kTenantIdPrefix + testNum++;
}

/**
 * Starts a migration from 'donorRst' and 'recipientRst', uses failCommand to force the
 * recipientSyncData command to fail with the given 'errorCode', and asserts the donor retries on
 * that error and is able to commit.
 */
function testDonorRetryRecipientSyncDataCmdOnError(tenantMigrationTest, errorCode, failMode) {
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const tenantId = makeTenantId();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    let fp = configureFailPoint(recipientPrimary,
                                "failCommand",
                                {
                                    failInternalCommands: true,
                                    errorCode: errorCode,
                                    failCommands: ["recipientSyncData"],
                                },
                                failMode);

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Verify that the command failed.
    const times = failMode.times ? failMode.times : 1;
    for (let i = 0; i < times; i++) {
        fp.wait();
    }
    fp.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    return migrationId;
}

/**
 * Starts a migration from 'donorRst' and 'recipientRst', uses failCommand to force the
 * recipientForgetMigration command to fail with the given 'errorCode', and asserts the donor
 * retries on that error and commits.
 */
function testDonorRetryRecipientForgetMigrationCmdOnError(tenantMigrationTest, errorCode) {
    const tenantId = makeTenantId();
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    let fp = configureFailPoint(recipientPrimary,
                                "failCommand",
                                {
                                    failInternalCommands: true,
                                    errorCode: errorCode,
                                    failCommands: ["recipientForgetMigration"],
                                },
                                {times: 1});

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    // Verify that the initial recipientForgetMigration command failed.
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    fp.wait();
    fp.off();

    // Check that forgetMigration properly deletes the stateDoc and mtab from the donor primary.
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);
}

(() => {
    jsTest.log(
        "Test that the donor retries recipientSyncData (to make the recipient start cloning) on recipient stepdown errors");
    const {tenantMigrationTest, teardown} = setup();

    const migrationId = testDonorRetryRecipientSyncDataCmdOnError(
        tenantMigrationTest, ErrorCodes.NotWritablePrimary, {times: 1});

    const configDonorsColl =
        tenantMigrationTest.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(TenantMigrationTest.DonorState.kCommitted,
              configDonorsColl.findOne({_id: migrationId}).state);
    assert.commandWorked(tenantMigrationTest.forgetMigration(extractUUIDFromObject(migrationId)));
    teardown();
})();

(() => {
    jsTest.log(
        "Test that the donor retries recipientSyncData (to make the recipient start cloning) on recipient shutdown errors");
    const {tenantMigrationTest, teardown} = setup();

    const migrationId = testDonorRetryRecipientSyncDataCmdOnError(
        tenantMigrationTest, ErrorCodes.ShutdownInProgress, {times: 1});

    const configDonorsColl =
        tenantMigrationTest.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(TenantMigrationTest.DonorState.kCommitted,
              configDonorsColl.findOne({_id: migrationId}).state);
    assert.commandWorked(tenantMigrationTest.forgetMigration(extractUUIDFromObject(migrationId)));
    teardown();
})();

(() => {
    jsTest.log(
        "Test that the donor retries recipientSyncData (with returnAfterReachingDonorTimestamp) on stepdown errors");
    const {tenantMigrationTest, teardown} = setup();

    const migrationId = testDonorRetryRecipientSyncDataCmdOnError(
        tenantMigrationTest, ErrorCodes.NotWritablePrimary, {skip: 1});

    const configDonorsColl =
        tenantMigrationTest.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(TenantMigrationTest.DonorState.kCommitted,
              configDonorsColl.findOne({_id: migrationId}).state);
    teardown();
})();

(() => {
    jsTest.log(
        "Test that the donor retries recipientSyncData (with returnAfterReachingDonorTimestamp) on recipient shutdown errors");
    const {tenantMigrationTest, teardown} = setup();

    const migrationId = testDonorRetryRecipientSyncDataCmdOnError(
        tenantMigrationTest, ErrorCodes.ShutdownInProgress, {skip: 1});

    const configDonorsColl =
        tenantMigrationTest.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(TenantMigrationTest.DonorState.kCommitted,
              configDonorsColl.findOne({_id: migrationId}).state);
    assert.commandWorked(tenantMigrationTest.forgetMigration(extractUUIDFromObject(migrationId)));
    teardown();
})();

(() => {
    jsTest.log("Test that the donor retries recipientForgetMigration on stepdown errors");
    const {tenantMigrationTest, teardown} = setup();
    testDonorRetryRecipientForgetMigrationCmdOnError(tenantMigrationTest,
                                                     ErrorCodes.NotWritablePrimary);
    teardown();
})();

(() => {
    jsTest.log("Test that the donor retries recipientForgetMigration on shutdown errors");
    const {tenantMigrationTest, teardown} = setup();
    testDonorRetryRecipientForgetMigrationCmdOnError(tenantMigrationTest,
                                                     ErrorCodes.ShutdownInProgress);
    teardown();
})();

(() => {
    jsTest.log("Test that the donor retries recipientForgetMigration on interruption errors");
    const {tenantMigrationTest, teardown} = setup();
    // Test an error code that is in the 'Interruption' category but not in the 'isRetriable'
    // category.
    const interruptionErrorCode = ErrorCodes.MaxTimeMSExpired;
    assert(ErrorCodes.isInterruption(interruptionErrorCode));
    testDonorRetryRecipientForgetMigrationCmdOnError(tenantMigrationTest, interruptionErrorCode);
    teardown();
})();

// Each donor state doc is updated three times throughout the lifetime of a tenant migration:
// - Set the "state" to "blocking"
// - Set the "state" to "commit"/"abort"
// - Set the "expireAt" to make the doc garbage collectable by the TTL index.
const kTotalStateDocUpdates = 3;
const kWriteErrorTimeMS = 50;

(() => {
    jsTest.log("Test that the donor retries state doc insert on retriable errors");

    const {tenantMigrationTest, teardown} = setup();
    const tenantId = makeTenantId();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    let fp = configureFailPoint(tenantMigrationTest.getDonorPrimary(), "failCollectionInserts", {
        collectionNS: TenantMigrationTest.kConfigDonorsNS,
    });

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

    // Start up a new thread to run this migration, since the 'failCollectionInserts' failpoint will
    // cause the initial 'donorStartMigration' command to loop forever without returning.
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();

    // Make the insert keep failing for some time.
    fp.wait();
    sleep(kWriteErrorTimeMS);
    fp.off();

    migrationThread.join();
    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    const configDonorsColl =
        tenantMigrationTest.getDonorPrimary().getCollection(TenantMigrationTest.kConfigDonorsNS);
    const donorStateDoc = configDonorsColl.findOne({_id: migrationId});
    assert.eq(TenantMigrationTest.DonorState.kCommitted, donorStateDoc.state);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    teardown();
})();

(() => {
    jsTest.log("Test that the donor retries state doc update on retriable errors");

    const {tenantMigrationTest, teardown} = setup();
    const tenantId = `${kTenantIdPrefix}RetryOnStateDocUpdateError`;

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

    // Use a random number of skips to fail a random update to config.tenantMigrationDonors.
    const fp = configureFailPoint(tenantMigrationTest.getDonorPrimary(),
                                  "failCollectionUpdates",
                                  {
                                      collectionNS: TenantMigrationTest.kConfigDonorsNS,
                                  },
                                  {skip: Math.floor(Math.random() * kTotalStateDocUpdates)});

    // Start up a new thread to run this migration, since we want to continuously send
    // 'donorStartMigration' commands while the 'failCollectionUpdates' failpoint is on.
    const migrationThread = new Thread((migrationOpts, donorRstArgs) => {
        load("jstests/replsets/libs/tenant_migration_util.js");
        assert.commandWorked(TenantMigrationUtil.runMigrationAsync(migrationOpts, donorRstArgs));
        assert.commandWorked(TenantMigrationUtil.forgetMigrationAsync(
            migrationOpts.migrationIdString, donorRstArgs));
    }, migrationOpts, donorRstArgs);
    migrationThread.start();

    // Make the update keep failing for some time.
    fp.wait();
    sleep(kWriteErrorTimeMS);
    fp.off();
    migrationThread.join();

    // The state docs will only be completed and marked as garbage collectable if the
    // update succeeds.
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);

    teardown();
})();
})();
