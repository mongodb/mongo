/**
 * Tests currentOp command during a tenant migration.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

// An object that mirrors the donor migration states.
const migrationStates = {
    kUninitialized: 0,
    kDataSync: 1,
    kBlocking: 2,
    kCommitted: 3,
    kAborted: 4
};

const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

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
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kDataSync);
    assert.eq(res.inprog[0].migrationCompleted, false);

    fp.off();
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in blocking state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
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
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kBlocking);
    assert(res.inprog[0].blockTimestamp);
    assert.eq(res.inprog[0].migrationCompleted, false);

    fp.off();
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for aborted migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kAborted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert(res.inprog[0].abortReason);
    assert.eq(res.inprog[0].migrationCompleted, false);
    tenantMigrationTest.stop();
})();

// Check currentOp while in committed state before and after a migration has completed.
(() => {
    jsTestLog("Testing currentOp output for committed migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));

    let res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert.eq(res.inprog[0].migrationCompleted, false);

    jsTestLog("Testing currentOp output for a committed migration after donorForgetMigration");

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert(res.inprog[0].expireAt);
    assert.eq(res.inprog[0].migrationCompleted, true);
    tenantMigrationTest.stop();
})();
})();
