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
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        }
    }
});

const kRecipientConnString = recipientRst.getURL();
const kTenantId = 'testTenantId';

recipientRst.startSet();
recipientRst.initiate();

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    donorRst.startSet();
    donorRst.initiate();
    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterDataSync");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);

    migrationThread.start();
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(res.inprog[0].recipientConnectionString, kRecipientConnString);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kDataSync);
    assert.eq(res.inprog[0].migrationCompleted, false);

    fp.off();
    migrationThread.join();
    donorRst.stopSet();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in blocking state");
    donorRst.startSet();
    donorRst.initiate();
    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);

    migrationThread.start();
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(res.inprog[0].recipientConnectionString, kRecipientConnString);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kBlocking);
    assert(res.inprog[0].blockTimestamp);
    assert.eq(res.inprog[0].migrationCompleted, false);

    fp.off();
    migrationThread.join();
    donorRst.stopSet();
})();

(() => {
    jsTestLog("Testing currentOp output for aborted migration");
    donorRst.startSet();
    donorRst.initiate();
    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };
    configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(res.inprog[0].recipientConnectionString, kRecipientConnString);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kAborted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert(res.inprog[0].abortReason);
    assert.eq(res.inprog[0].migrationCompleted, false);

    donorRst.stopSet();
})();

// Check currentOp while in committed state before and after a migration has completed.
(() => {
    jsTestLog("Testing currentOp output for committed migration");
    donorRst.startSet();
    donorRst.initiate();
    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };
    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));

    let res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(res.inprog[0].recipientConnectionString, kRecipientConnString);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert.eq(res.inprog[0].migrationCompleted, false);

    jsTestLog("Testing currentOp output for a committed migration after donorForgetMigration");

    assert.commandWorked(
        TenantMigrationUtil.forgetMigration(donorPrimary.host, migrationOpts.migrationIdString));

    res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, migrationId), 0);
    assert.eq(res.inprog[0].recipientConnectionString, kRecipientConnString);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].blockTimestamp);
    assert(res.inprog[0].commitOrAbortOpTime);
    assert(res.inprog[0].expireAt);
    assert.eq(res.inprog[0].migrationCompleted, true);

    donorRst.stopSet();
})();

recipientRst.stopSet();
})();
