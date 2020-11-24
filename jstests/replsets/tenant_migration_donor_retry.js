/**
 * Tests that the donor retries its steps until success, or it gets an error that should lead to
 * an abort decision.
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

const kTenantIdPrefix = "testTenantId";
let testNum = 0;

// TODO SERVER-53107: Remove 'enableRecipientTesting: false'.
const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

function makeTenantId() {
    return kTenantIdPrefix + testNum++;
}

/**
 * Starts a migration from 'donorRst' and 'recipientRst', uses failCommand to force the
 * recipientSyncData command to fail with the given 'errorCode', and asserts the donor does not
 * retry on that error and aborts the migration.
 *
 * TODO: This function should be changed to testDonorRetryRecipientSyncDataCmdOnError once there is
 * a way to differentiate between local and remote stepdown/shutdown error.
 */
function testMigrationAbortsOnRecipientSyncDataCmdError(errorCode, failMode) {
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

    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, errorCode);

    return migrationId;
}

/**
 * Starts a migration from 'donorRst' and 'recipientRst', uses failCommand to force the
 * recipientForgetMigration command to fail with the given 'errorCode', and asserts the donor does
 * not retry on that error and aborts the migration.
 *
 * TODO: This function should be changed to testDonorRetryRecipientForgetMigrationCmdOnError once
 * there is a way to differentiate between local and remote stepdown/shutdown error.
 */
function testMigrationAbortsOnRecipientForgetMigrationCmdError(errorCode) {
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

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));

    // Verify that the initial recipientForgetMigration command failed.
    assert.commandFailedWithCode(
        tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString), errorCode);
    fp.wait();
    fp.off();

    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert(!stateRes.expireAt);
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (to make the recipient start cloning)" +
        " on recipient stepdown errors");

    const migrationId =
        testMigrationAbortsOnRecipientSyncDataCmdError(ErrorCodes.NotWritablePrimary, {times: 1});

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert(!configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq(TenantMigrationTest.State.kAborted,
              configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (to make the recipient start cloning)" +
        " on recipient shutdown errors");

    const migrationId =
        testMigrationAbortsOnRecipientSyncDataCmdError(ErrorCodes.ShutdownInProgress, {times: 1});

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert(!configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq(TenantMigrationTest.State.kAborted,
              configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (with returnAfterReachingDonorTimestamp) " +
        "on stepdown errors");

    const migrationId =
        testMigrationAbortsOnRecipientSyncDataCmdError(ErrorCodes.NotWritablePrimary, {skip: 1});

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert(configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq(TenantMigrationTest.State.kAborted,
              configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (with returnAfterReachingDonorTimestamp) " +
        "on recipient shutdown errors");

    const migrationId =
        testMigrationAbortsOnRecipientSyncDataCmdError(ErrorCodes.ShutdownInProgress, {skip: 1});

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert(configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq(TenantMigrationTest.State.kAborted,
              configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log("Test that the donor does not retry recipientForgetMigration on stepdown errors");
    testMigrationAbortsOnRecipientForgetMigrationCmdError(ErrorCodes.NotWritablePrimary);
})();

(() => {
    jsTest.log("Test that the donor does not retry recipientForgetMigration on shutdown errors");
    testMigrationAbortsOnRecipientForgetMigrationCmdError(ErrorCodes.ShutdownInProgress);
})();

// Each donor state doc is updated three times throughout the lifetime of a tenant migration:
// - Set the "state" to "blocking"
// - Set the "state" to "commit"/"abort"
// - Set the "expireAt" to make the doc garbage collectable by the TTL index.
const kTotalStateDocUpdates = 3;
const kWriteErrorTimeMS = 50;

(() => {
    jsTest.log("Test that the donor retries state doc insert on retriable errors");

    const tenantId = makeTenantId();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    let fp = configureFailPoint(donorPrimary, "failCollectionInserts", {
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
    assert.commandWorked(migrationThread.returnData());

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const donorStateDoc = configDonorsColl.findOne({_id: migrationId});
    assert.eq(TenantMigrationTest.State.kCommitted, donorStateDoc.state);
})();

(() => {
    jsTest.log("Test that the donor retries state doc update on retriable errors");

    const tenantId = kTenantIdPrefix + "RetryOnStateDocUpdateError";

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

    // Use a random number of skips to fail a random update to config.tenantMigrationDonors.
    const fp = configureFailPoint(donorPrimary,
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

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const donorStateDoc = configDonorsColl.findOne({_id: migrationId});
    assert.eq(donorStateDoc.state, TenantMigrationTest.State.kCommitted);
    assert(donorStateDoc.expireAt);
})();

tenantMigrationTest.stop();
})();
