/**
 * Tests that writes that are blocked during a tenant migration will still be able to find out
 * migration outcome even if the migration's in memory state has been garbage collected.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// Set the delay before a donor state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;
const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
        }
    }
});

function insertDocument(primaryHost, dbName, collName) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let res = primaryDB.runCommand({insert: collName, documents: [{x: 1}]});
    return res;
}

(() => {
    jsTestLog(
        "Testing blocked writes can see migration outcome for a migration that has been committed and garbage collected.");

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, enableRecipientTesting: false});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const tenantId = "migrationOutcome-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kCommitted);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    const writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationCommitted);

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();

(() => {
    jsTestLog(
        "Testing blocked writes can see migration outcome for a migration that has been aborted and garbage collected.");

    donorRst.startSet();
    donorRst.initiate();

    // TODO SERVER-XXXX: Remove 'enableRecipientTesting: false'.
    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, enableRecipientTesting: false});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const tenantId = "migrationOutcome-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kAborted);
    abortFp.off();

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    const writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();
})();
