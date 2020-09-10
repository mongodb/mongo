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

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and asserts that the command either succeeds or fails with an
 * expected error.
 */
function testDonorStartMigrationInterrupt(interruptFunc, isExpectedErrorFunc) {
    const donorRst = new ReplSetTest(
        {nodes: 1, name: "donorRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});
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
    interruptFunc(donorRst);
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
function testDonorForgetMigrationInterrupt(interruptFunc, isExpectedErrorFunc) {
    const donorRst = new ReplSetTest(
        {nodes: 1, name: "donorRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});
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

    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    let forgetMigrationThread = new Thread(
        TenantMigrationUtil.forgetMigration, donorPrimary.host, migrationOpts.migrationIdString);
    forgetMigrationThread.start();
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);
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
})();
