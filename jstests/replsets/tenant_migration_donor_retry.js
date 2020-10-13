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
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";
let testNum = 0;
const kConfigDonorsNS = "config.tenantMigrationDonors";

function makeTenantId() {
    return kTenantId + testNum++;
}

/**
 * Starts a migration from 'donorRst' and 'recipientRst', uses failCommand to force the
 * recipientSyncData command to fail with the given 'errorCode', and asserts the donor does not
 * retry on that error and aborts the migration.
 *
 * TODO: This function should be changed to testDonorRetryRecipientSyncDataCmdOnError once there is
 * a way to differentiate between local and remote stepdown/shutdown error.
 */
function testMigrationAbortsOnRecipientSyncDataCmdError(
    donorRst, recipientRst, errorCode, failMode) {
    const donorPrimary = donorRst.getPrimary();
    const recipientPrimary = recipientRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId + makeTenantId(),
        readPreference: {mode: "primary"},
    };

    let fp = configureFailPoint(recipientPrimary,
                                "failCommand",
                                {
                                    failInternalCommands: true,
                                    errorCode: errorCode,
                                    failCommands: ["recipientSyncData"],
                                },
                                failMode);

    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    migrationThread.start();

    // Verify that the command failed.
    const times = failMode.times ? failMode.times : 1;
    for (let i = 0; i < times; i++) {
        fp.wait();
    }
    fp.off();

    migrationThread.join();
    const res = assert.commandWorked(migrationThread.returnData());
    assert.eq(res.state, "aborted");
    assert.eq(res.abortReason.code, errorCode);

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
function testMigrationAbortsOnRecipientForgetMigrationCmdError(donorRst, recipientRst, errorCode) {
    const donorPrimary = donorRst.getPrimary();
    const recipientPrimary = recipientRst.getPrimary();
    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: makeTenantId(),
        readPreference: {mode: "primary"},
    };

    let fp = configureFailPoint(recipientPrimary,
                                "failCommand",
                                {
                                    failInternalCommands: true,
                                    errorCode: errorCode,
                                    failCommands: ["recipientForgetMigration"],
                                },
                                {times: 1});

    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    let forgetMigrationThread = new Thread(
        TenantMigrationUtil.forgetMigration, donorPrimary.host, migrationOpts.migrationIdString);
    forgetMigrationThread.start();

    // Verify that the initial recipientForgetMigration command failed.
    fp.wait();

    forgetMigrationThread.join();
    assert.commandFailedWithCode(forgetMigrationThread.returnData(), errorCode);
    fp.off();

    const donorStateDoc = configDonorsColl.findOne({_id: migrationId});
    assert.eq("committed", donorStateDoc.state);
    assert(!donorStateDoc.expireAt);
}

const donorRst = new ReplSetTest(
    {nodes: 1, name: "donorRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: "recipientRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const donorPrimary = donorRst.getPrimary();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (to make the recipient start cloning)" +
        " on recipient stepdown errors");

    const migrationId = testMigrationAbortsOnRecipientSyncDataCmdError(
        donorRst, recipientRst, ErrorCodes.NotWritablePrimary, {times: 1});

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert(!configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq("aborted", configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (to make the recipient start cloning)" +
        " on recipient shutdown errors");

    const migrationId = testMigrationAbortsOnRecipientSyncDataCmdError(
        donorRst, recipientRst, ErrorCodes.ShutdownInProgress, {times: 1});

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert(!configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq("aborted", configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (with returnAfterReachingTimestamp) " +
        "on stepdown errors");

    const migrationId = testMigrationAbortsOnRecipientSyncDataCmdError(
        donorRst, recipientRst, ErrorCodes.NotWritablePrimary, {skip: 1});

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert(configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq("aborted", configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log(
        "Test that the donor does not retry recipientSyncData (with returnAfterReachingTimestamp) " +
        "on recipient shutdown errors");

    const migrationId = testMigrationAbortsOnRecipientSyncDataCmdError(
        donorRst, recipientRst, ErrorCodes.ShutdownInProgress, {skip: 1});

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert(configDonorsColl.findOne({_id: migrationId}).blockTimestamp);
    assert.eq("aborted", configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log("Test that the donor does not retry recipientForgetMigration on stepdown errors");
    testMigrationAbortsOnRecipientForgetMigrationCmdError(
        donorRst, recipientRst, ErrorCodes.NotWritablePrimary);
})();

(() => {
    jsTest.log("Test that the donor does not retry recipientForgetMigration on shutdown errors");

    testMigrationAbortsOnRecipientForgetMigrationCmdError(
        donorRst, recipientRst, ErrorCodes.ShutdownInProgress);
})();

// Each donor state doc is updated three times throughout the lifetime of a tenant migration:
// - Set the "state" to "blocking"
// - Set the "state" to "commit"/"abort"
// - Set the "expireAt" to make the doc garbage collectable by the TTL index.
const kTotalStateDocUpdates = 3;
const kWriteErrorTimeMS = 50;

(() => {
    jsTest.log("Test that the donor retries state doc insert on retriable errors");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: makeTenantId(),
        readPreference: {mode: "primary"},
    };

    let fp = configureFailPoint(donorPrimary, "failCollectionInserts", {
        collectionNS: kConfigDonorsNS,
    });

    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    migrationThread.start();

    // Make the insert keep failing for some time.
    fp.wait();
    sleep(kWriteErrorTimeMS);
    fp.off();

    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert.eq("committed", configDonorsColl.findOne({_id: migrationId}).state);
})();

(() => {
    jsTest.log("Test that the donor retries state doc update on retriable errors");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId + "RetryOnStateDocUpdateError",
        readPreference: {mode: "primary"},
    };

    // Use a random number of skips to fail a random update to config.tenantMigrationDonors.
    let fp = configureFailPoint(donorPrimary,
                                "failCollectionUpdates",
                                {
                                    collectionNS: kConfigDonorsNS,
                                },
                                {skip: Math.floor(Math.random() * kTotalStateDocUpdates)});

    let migrationThread = new Thread((donorPrimaryHost, migrationOpts) => {
        load("jstests/replsets/libs/tenant_migration_util.js");
        assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimaryHost, migrationOpts));
        assert.commandWorked(
            TenantMigrationUtil.forgetMigration(donorPrimaryHost, migrationOpts.migrationIdString));
    }, donorPrimary.host, migrationOpts);
    migrationThread.start();

    // Make the update keep failing for some time.
    fp.wait();
    sleep(kWriteErrorTimeMS);
    fp.off();
    migrationThread.join();

    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    const donorStateDoc = configDonorsColl.findOne({_id: migrationId});
    assert.eq("committed", donorStateDoc.state);
    assert(donorStateDoc.expireAt);
})();

donorRst.stopSet();
recipientRst.stopSet();
})();
