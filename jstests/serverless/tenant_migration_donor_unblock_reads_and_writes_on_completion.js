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
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    getNumBlockedReads,
    getNumBlockedWrites,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

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
        serverless: true,
        nodeOptions: Object.assign(makeX509OptionsForTest().donor, {
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

const kDbName = "testDb";
const kCollName = "testColl";

(() => {
    jsTest.log(
        "Test that a lagged donor secondary correctly unblocks blocked reads after the migration aborts");
    const {tenantMigrationTest, donorRst, teardown} = setup();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    const tenantId = ObjectId().str;
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
    const donorDoc = donorsColl.findOne({_id: migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockTimestamp);
    assert.soon(() => getNumBlockedReads(laggedSecondary, tenantId) == 1,
                "Tenant " + tenantId + " of " + dbName + " db in collection " + kCollName +
                    " received " + getNumBlockedReads(laggedSecondary, tenantId) +
                    " blocked reads. Expected 1\n");

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
    const tenantId = ObjectId().str;
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
    const donorDoc = donorsColl.findOne({_id: migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockTimestamp);
    assert.soon(() => getNumBlockedReads(laggedSecondary, tenantId) == 1,
                "Tenant " + tenantId + " of " + dbName + " db in collection " + kCollName +
                    " received " + getNumBlockedReads(laggedSecondary, tenantId) +
                    " blocked reads. Expected 1\n");

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
    const tenantId = ObjectId().str;
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
    const donorDoc = donorsColl.findOne({_id: migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(donorPrimary, dbName, kCollName, donorDoc.blockTimestamp);
    const writeThread = startWriteThread(donorPrimary, dbName, kCollName);
    assert.soon(() => getNumBlockedReads(donorPrimary, tenantId) == 1,
                "Tenant " + tenantId + " of " + dbName + " db in collection " + kCollName +
                    " recieved " + getNumBlockedReads(donorPrimary, tenantId) +
                    " blocked reads. Expected 1\n");
    assert.soon(() => getNumBlockedWrites(donorPrimary, tenantId) == 1,
                "Tenant " + tenantId + " of " + dbName + " db in collection " + kCollName +
                    " recieved " + getNumBlockedWrites(donorPrimary, tenantId) +
                    " blocked writes. Expected 1\n");

    // Cannot delete the donor state doc since it has not been marked as garbage collectable.
    assert.commandFailedWithCode(donorsColl.remove({}), ErrorCodes.IllegalOperation);

    // Cannot mark the state doc as garbage collectable before the migration commits or aborts.
    assert.commandFailedWithCode(
        donorsColl.update({_id: migrationId}, {$set: {expireAt: new Date()}}), ErrorCodes.BadValue);

    // Can drop the state doc collection but this will not cause all blocked reads and writes to
    // hang.
    assert(donorsColl.drop());
    assert.commandFailedWithCode(readThread.returnData(), ErrorCodes.Interrupted);
    assert.commandFailedWithCode(writeThread.returnData(), ErrorCodes.Interrupted);
    blockingFp.off();

    teardown();
})();
