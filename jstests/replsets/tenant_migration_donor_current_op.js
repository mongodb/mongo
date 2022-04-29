/**
 * Tests currentOp command during a tenant migration.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
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
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// An object that mirrors the donor migration states.
const migrationStates = {
    kUninitialized: 0,
    kAbortingIndexBuilds: 1,
    kDataSync: 2,
    kBlocking: 3,
    kCommitted: 4,
    kAborted: 5
};

const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};

function checkStandardFieldsOK(ops, {
    migrationId,
    lastDurableState,
    tenantMigrationTest,
    migrationCompleted = false,
}) {
    assert.eq(ops.length, 1);
    const [op] = ops;
    assert.eq(bsonWoCompare(op.instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(op.readPreference, kReadPreference), 0);
    assert.eq(op.lastDurableState, lastDurableState);
    assert.eq(op.migrationCompleted, migrationCompleted);
    assert(op.migrationStart instanceof Date);
    assert.eq(op.recipientConnectionString, tenantMigrationTest.getRecipientRst().getURL());

    if (TenantMigrationUtil.isShardMergeEnabled(
            tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
        assert.eq(op.tenantId, undefined);
    } else {
        assert.eq(bsonWoCompare(op.tenantId, kTenantId), 0);
    }
}

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary,
                                "pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kAbortingIndexBuilds,
        tenantMigrationTest,
    });

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kDataSync,
        tenantMigrationTest,
    });
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in blocking state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kBlocking,
        tenantMigrationTest,
    });
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for aborted migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kAborted,
        tenantMigrationTest,
    });
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert.eq(typeof res.inprog[0].abortReason.code, "number");
    assert.eq(typeof res.inprog[0].abortReason.codeName, "string");
    assert.eq(typeof res.inprog[0].abortReason.errmsg, "string");

    tenantMigrationTest.stop();
})();

// Check currentOp while in committed state before and after a migration has completed.
(() => {
    jsTestLog("Testing currentOp output for committed migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    assert.commandWorked(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    let res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kCommitted,
        tenantMigrationTest,
    });
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);

    jsTestLog("Testing currentOp output for a committed migration after donorForgetMigration");

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});

    checkStandardFieldsOK(res.inprog, {
        migrationId,
        lastDurableState: migrationStates.kCommitted,
        tenantMigrationTest,
        migrationCompleted: true,
    });
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert(res.inprog[0].expireAt instanceof Date);

    tenantMigrationTest.stop();
})();
})();
