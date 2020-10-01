/**
 * Tests current op command during a tenant migration.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// An object that mirrors the donor migration states.
const migrationStates = {
    kUninitialized: 0,
    kDataSync: 1,
    kBlocking: 2,
    kCommitted: 3,
    kAborted: 4
};

const donorRst = new ReplSetTest(
    {nodes: 1, name: 'donor', nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: 'recipient', nodeOptions: {setParameter: {enableTenantMigrations: true}}});

const kDBPrefix = 'testDb';

recipientRst.startSet();
recipientRst.initiate();

function testCurrentOpOutputForInProgressMigration(testParams) {
    jsTestLog("Testing current op output for in progress migration.");

    donorRst.startSet();
    donorRst.initiate();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        dbPrefix: kDBPrefix,
        readPreference: {mode: "primary"},
    };

    const donorPrimary = donorRst.getPrimary();
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    let fp = configureFailPoint(donorPrimary, testParams.failpoint);

    migrationThread.start();
    fp.wait();

    const currOp = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(currOp.inprog.length, 1);
    assert.eq(currOp.inprog[0].lastDurableState, testParams.expectedState);
    assert.eq(currOp.inprog[0].migrationCompleted, false);

    fp.off();
    migrationThread.join();
    donorRst.stopSet();
}

function testCurrentOpOutputForCommittedMigration(testParams) {
    jsTestLog("Testing current op output for s committed migration.");

    donorRst.startSet();
    donorRst.initiate();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        dbPrefix: kDBPrefix,
        readPreference: {mode: "primary"},
    };

    const donorPrimary = donorRst.getPrimary();

    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));

    let currOp = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(currOp.inprog.length, 1);
    assert.eq(currOp.inprog[0].lastDurableState, testParams.expectedState);
    assert.eq(currOp.inprog[0].migrationCompleted, false);

    jsTestLog("Migration has successfully committed.");

    assert.commandWorked(
        TenantMigrationUtil.forgetMigration(donorPrimary.host, migrationOpts.migrationIdString));
    currOp = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(currOp.inprog.length, 1);
    assert.eq(currOp.inprog[0].lastDurableState, testParams.expectedState);
    assert.eq(currOp.inprog[0].migrationCompleted, true);

    donorRst.stopSet();
}

// Check current op while in data sync state.
(() => {
    let testParams = {
        failpoint: "pauseTenantMigrationAfterDataSync",
        expectedState: migrationStates.kDataSync
    };
    testCurrentOpOutputForInProgressMigration(testParams);
})();

// Check current op while in blocking state.
(() => {
    let testParams = {
        failpoint: "pauseTenantMigrationAfterBlockingStarts",
        expectedState: migrationStates.kBlocking
    };
    testCurrentOpOutputForInProgressMigration(testParams);
})();

// Check current op while in aborted state.
(() => {
    let testParams = {
        failpoint: "abortTenantMigrationAfterBlockingStarts",
        expectedState: migrationStates.kAborted
    };
    testCurrentOpOutputForInProgressMigration(testParams);
})();

// Check current op while in committed state before and after a migration has completed.
(() => {
    let testParams = {expectedState: migrationStates.kCommitted};
    testCurrentOpOutputForCommittedMigration(testParams);
})();

recipientRst.stopSet();
})();
