/**
 * Tests that the migration is interrupted successfully on stepdown and shutdown.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxSleepTimeMS = 1000;
const kConfigDonorsNS = "config.tenantMigrationDonors";

// Set the delay before a donor state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionDelayMS = 60 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and asserts that the command either succeeds or fails with an
 * expected error.
 */
function testDonorStartMigrationInterrupt(interruptFunc, isExpectedErrorFunc, numDonorRsNodes = 1) {
    const donorRst = new ReplSetTest({
        nodes: numDonorRsNodes,
        name: "donorRst",
        nodeOptions: {setParameter: {enableTenantMigrations: true}}
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: {setParameter: {enableTenantMigrations: true}}
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        dbPrefix: "testDbPrefix",
        readPreference: {mode: "primary"},
    };

    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    migrationThread.start();
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst, migrationId);
    migrationThread.join();

    const res = migrationThread.returnData();
    assert(res.ok || isExpectedErrorFunc(res.code), tojson(res));

    donorRst.stopSet();
    recipientRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the donor using the 'interruptFunc', and asserts that the command either succeeds or fails with
 * an expected error.
 */
function testDonorForgetMigrationInterrupt(
    interruptFunc, isExpectedErrorFunc, numDonorRsNodes = 1) {
    const donorRst = new ReplSetTest({
        nodes: numDonorRsNodes,
        name: "donorRst",
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        }
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        }
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        dbPrefix: "testDbPrefix",
        readPreference: {mode: "primary"},
    };

    donorPrimary.getCollection(kConfigDonorsNS).createIndex({expireAt: 1}, {expireAfterSeconds: 0});

    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    let forgetMigrationThread = new Thread(
        TenantMigrationUtil.forgetMigration, donorPrimary.host, migrationOpts.migrationIdString);
    forgetMigrationThread.start();
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst, migrationId);
    forgetMigrationThread.join();

    const res = forgetMigrationThread.returnData();
    assert(res.ok || isExpectedErrorFunc(res.code), tojson(res));

    donorRst.stopSet();
    recipientRst.stopSet();
}

(() => {
    jsTest.log("Test that the donorStartMigration command is interrupted successfully on stepdown");
    testDonorStartMigrationInterrupt((donorRst) => {
        assert.commandWorked(
            donorRst.getPrimary().adminCommand({replSetStepDown: 1000, force: true}));
    }, (errorCode) => ErrorCodes.isNotPrimaryError(errorCode));
})();

(() => {
    jsTest.log("Test that the donorStartMigration command is interrupted successfully on shutdown");
    testDonorStartMigrationInterrupt(
        (donorRst) => {
            donorRst.stopSet();
        },
        (errorCode) =>
            ErrorCodes.isNotPrimaryError(errorCode) || ErrorCodes.isShutdownError(errorCode));
})();

(() => {
    jsTest.log("Test that the donorForgetMigration is interrupted successfully on stepdown");
    testDonorForgetMigrationInterrupt((donorRst) => {
        assert.commandWorked(
            donorRst.getPrimary().adminCommand({replSetStepDown: 1000, force: true}));
    }, (errorCode) => ErrorCodes.isNotPrimaryError(errorCode));
})();

(() => {
    jsTest.log("Test that the donorForgetMigration is interrupted successfully on shutdown");
    testDonorForgetMigrationInterrupt(
        (donorRst) => {
            donorRst.stopSet();
        },
        (errorCode) =>
            ErrorCodes.isNotPrimaryError(errorCode) || ErrorCodes.isShutdownError(errorCode));
})();

(() => {
    jsTest.log("Test that the donorStartMigration command resumes on stepup");
    testDonorStartMigrationInterrupt((donorRst, migrationId) => {
        let donorPrimary = donorRst.getPrimary();

        // Use a short replSetStepDown seconds to make it more likely for the old primary to step
        // back up.
        assert.commandWorked(donorPrimary.adminCommand({replSetStepDown: 1, force: true}));

        donorPrimary = donorRst.getPrimary();

        const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
        if (configDonorsColl.count({_id: migrationId}) > 0) {
            // If the donor's primary steps down before inserting the donor state doc, then there
            // is no migration to resume.
            assert.soon(() => {
                return "committed" === configDonorsColl.findOne({_id: migrationId}).state;
            });
        }
    }, (errorCode) => ErrorCodes.isNotPrimaryError(errorCode), 3 /* numDonorRsNodes */);
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried on stepup");
    testDonorForgetMigrationInterrupt((donorRst, migrationId) => {
        let donorPrimary = donorRst.getPrimary();

        // Use a short replSetStepDown seconds to make it more likely for the old primary to step
        // back up.
        assert.commandWorked(donorRst.getPrimary().adminCommand({replSetStepDown: 1, force: true}));

        donorPrimary = donorRst.getPrimary();

        assert.commandWorked(TenantMigrationUtil.forgetMigration(
            donorPrimary.host, extractUUIDFromObject(migrationId)));

        assert.soon(() => 0 ===
                        donorPrimary.getCollection(kConfigDonorsNS).count({_id: migrationId}));
        assert.soon(() => 0 ===
                        donorPrimary.adminCommand({serverStatus: 1})
                            .repl.primaryOnlyServices.TenantMigrationDonorService);
    }, (errorCode) => ErrorCodes.isNotPrimaryError(errorCode), 3 /* numDonorRsNodes */);
})();
})();
