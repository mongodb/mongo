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
            enableTenantMigrations: true,
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
        }
    }
});
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        },
    }
});
const kRecipientConnString = recipientRst.getURL();

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
    recipientRst.startSet();
    recipientRst.initiate();

    let dbName = "migrationOutcome-committed_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    let writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    let writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    const migrationId = UUID();
    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "committed");

    assert.commandWorked(
        TenantMigrationUtil.forgetMigration(primary.host, migrationOpts.migrationIdString));
    TenantMigrationUtil.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    let writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationCommitted);

    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog(
        "Testing blocked writes can see migration outcome for a migration that has been aborted and garbage collected.");

    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    let dbName = "migrationOutcome-aborted_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    let writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    let writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    const migrationId = UUID();
    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");
    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "aborted");
    abortFp.off();

    assert.commandWorked(
        TenantMigrationUtil.forgetMigration(primary.host, migrationOpts.migrationIdString));
    TenantMigrationUtil.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    let writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationAborted);

    donorRst.stopSet();
    recipientRst.stopSet();
})();
})();
