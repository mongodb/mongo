/**
 * Tests that tenant migration donor reliably unblocks blocked reads and writes when the migration
 * completes or is interrupted when the state doc collection is dropped.
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

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

function startReadThread(node, dbName, collName, afterClusterTime) {
    let readThread = new Thread((host, dbName, collName, afterClusterTime) => {
        const node = new Mongo(host);
        node.setSecondaryOk();
        const db = node.getDB(dbName);
        return db.runCommand({
            find: collName,
            readConcern: {afterClusterTime: Timestamp(afterClusterTime.t, afterClusterTime.i)}
        });
    }, node.host, dbName, collName, afterClusterTime);
    readThread.start();
    return readThread;
}

function startWriteThread(node, dbName, collName) {
    let writeThread = new Thread((host, dbName, collName) => {
        const node = new Mongo(host);
        const db = node.getDB(dbName);
        return db.runCommand({insert: collName, documents: [{_id: 1}]});
    }, node.host, dbName, collName);
    writeThread.start();
    return writeThread;
}

function setup() {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: 1,
                ttlMonitorSleepSecs: 1,
            }
        }),
        // Disallow chaining to force both secondaries to sync from the primary. One of the test
        // cases below disables replication on one of the secondaries, with chaining it would
        // effectively disable replication on both secondaries, causing the migration to hang since
        // majority write concern is unsatsifiable.
        settings: {chainingAllowed: false}
    });
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});

    return {
        donorRst,
        tenantMigrationTest,
        teardown: () => {
            donorRst.stopSet();
            tenantMigrationTest.stop();
        }
    };
}

const kTenantIdPrefix = "testTenantId";
const kDbName = "testDb";
const kCollName = "testColl";

(() => {
    jsTest.log(
        "Test that a lagged donor secondary correctly unblocks blocked reads after the migration aborts");
    const {tenantMigrationTest, donorRst, teardown} = setup();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const tenantId = kTenantIdPrefix + "LaggedSecondaryMigrationAborted";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    blockingFp.wait();
    donorRst.awaitReplication();

    // Run a read command against one of the secondaries, and wait for it to block.
    const laggedSecondary = donorRst.getSecondary();
    const donorDoc = donorsColl.findOne({tenantId: tenantId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockTimestamp);
    assert.soon(() => TenantMigrationUtil.getNumBlockedReads(laggedSecondary, tenantId) == 1);

    // Disable snapshotting on that secondary, and wait for the migration to abort and be garbage
    // collected. That way the secondary is guaranteed to observe the write to set expireAt before
    // learning that the abortOpTime has been majority committed.
    let snapshotFp = configureFailPoint(laggedSecondary, "disableSnapshotting");
    blockingFp.off();
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(
        migrationId, tenantId, [donorPrimary] /* donorNodes */, [] /* recipientNodes */);

    assert.commandWorked(readThread.returnData());
    abortFp.off();
    snapshotFp.off();

    teardown();
})();

(() => {
    jsTest.log(
        "Test that a lagged donor secondary correctly unblocks blocked reads after the migration commits");
    const {tenantMigrationTest, donorRst, teardown} = setup();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const tenantId = kTenantIdPrefix + "LaggedSecondaryMigrationCommitted";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    blockingFp.wait();
    donorRst.awaitReplication();

    // Run a read command against one of the secondaries, and wait for it to block.
    const laggedSecondary = donorRst.getSecondary();
    const donorDoc = donorsColl.findOne({tenantId: tenantId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockTimestamp);
    assert.soon(() => TenantMigrationUtil.getNumBlockedReads(laggedSecondary, tenantId) == 1);

    // Disable snapshotting on that secondary, and wait for the migration to commit and be garbage
    // collected. That way the secondary is guaranteed to observe the write to set expireAt before
    // learning that the commitOpTime has been majority committed.
    let snapshotFp = configureFailPoint(laggedSecondary, "disableSnapshotting");
    blockingFp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(
        migrationId, tenantId, [donorPrimary] /* donorNodes */, [] /* recipientNodes */);

    assert.commandFailedWithCode(readThread.returnData(), ErrorCodes.TenantMigrationCommitted);
    snapshotFp.off();

    teardown();
})();

(() => {
    jsTest.log(
        "Test that blocked writes and reads are interrupted when the donor's state doc collection is dropped");
    const {tenantMigrationTest, donorRst, teardown} = setup();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const tenantId = kTenantIdPrefix + "DropStateDocCollection";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    blockingFp.wait();

    // Run a read command and a write command against the primary, and wait for them to block.
    const donorDoc = donorsColl.findOne({tenantId: tenantId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(donorPrimary, dbName, kCollName, donorDoc.blockTimestamp);
    const writeThread = startWriteThread(donorPrimary, dbName, kCollName);
    assert.soon(() => TenantMigrationUtil.getNumBlockedReads(donorPrimary, tenantId) == 1);
    assert.soon(() => TenantMigrationUtil.getNumBlockedWrites(donorPrimary, tenantId) == 1);

    // Cannot delete the donor state doc since it has not been marked as garbage collectable.
    assert.commandFailedWithCode(donorsColl.remove({}), ErrorCodes.IllegalOperation);

    // Cannot mark the state doc as garbage collectable before the migration commits or aborts.
    assert.commandFailedWithCode(
        donorsColl.update({tenantId: tenantId}, {$set: {expireAt: new Date()}}),
        ErrorCodes.BadValue);

    // Can drop the state doc collection but this will not cause all blocked reads and writes to
    // hang.
    assert(donorsColl.drop());
    assert.commandFailedWithCode(readThread.returnData(), ErrorCodes.Interrupted);
    assert.commandFailedWithCode(writeThread.returnData(), ErrorCodes.Interrupted);
    blockingFp.off();

    teardown();
})();
})();
