/**
 * Tests that retryable writes made on the donor during a tenant migration can be properly retried
 * on the recipient for time-series collections.
 *
 * This test is based on "tenant_migration_retryable_write_retry_on_recipient.js".
 *
 * TODO (SERVER-68159) we should no longer need to use the incompatible_with_shard_merge. Remove it.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   incompatible_with_shard_merge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/libs/uuid_util.js");

function testRetryOnRecipient(ordered) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const kTenantId = "testTenantId";
    const kDbName = tenantMigrationTest.tenantDB(kTenantId, "tsDb");
    const kCollNameBefore = "tsCollBefore";
    const kCollNameDuring = "tsCollDuring";

    const donorDb = donorPrimary.getDB(kDbName);
    assert.commandWorked(donorDb.createCollection(
        kCollNameBefore, {timeseries: {timeField: "time", metaField: "meta"}}));
    assert.commandWorked(donorDb.createCollection(
        kCollNameDuring, {timeseries: {timeField: "time", metaField: "meta"}}));
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientDb = recipientPrimary.getDB(kDbName);

    const pauseTenantMigrationBeforeLeavingDataSyncState =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: kTenantId,
    };
    function setupRetryableWritesForCollection(collName) {
        const kNs = `${kDbName}.${collName}`;
        assert.commandWorked(donorPrimary.getCollection(kNs).insert(
            [
                {time: ISODate(), x: 0, meta: 0},
                {time: ISODate(), x: 1, meta: 0},
                {time: ISODate(), x: 2, meta: 0},
            ],
            {writeConcern: {w: "majority"}}));

        let result = {collName: collName};
        const lsid1 = {id: UUID()};
        const insertTag = "retryable insert " + collName;
        const updateTag = "retryable update " + collName;
        result.insertTag = insertTag;
        result.updateTag = updateTag;
        result.retryableInsertCommand = {
            insert: collName,
            documents: [
                // Batched inserts resulting in "inserts".
                {x: 0, time: ISODate(), tag: insertTag, meta: 1},
                {x: 1, time: ISODate(), tag: insertTag, meta: 1},
                {x: 2, time: ISODate(), tag: insertTag, meta: 1},
                // Batched inserts resulting in "updates".
                {x: 3, time: ISODate(), tag: updateTag, meta: 0},
                {x: 4, time: ISODate(), tag: updateTag, meta: 0},
                {x: 5, time: ISODate(), tag: updateTag, meta: 0},
            ],
            txnNumber: NumberLong(0),
            lsid: lsid1,
            ordered: ordered,
        };
        return result;
    }

    const beforeWrites = setupRetryableWritesForCollection(kCollNameBefore);
    const duringWrites = setupRetryableWritesForCollection(kCollNameDuring);

    jsTestLog("Run retryable writes before the migration");
    assert.commandWorked(donorDb.runCommand(beforeWrites.retryableInsertCommand));

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();

    pauseTenantMigrationBeforeLeavingDataSyncState.wait();

    jsTestLog("Run retryable writes during the migration");
    assert.commandWorked(donorDb.runCommand(duringWrites.retryableInsertCommand));

    // Wait for the migration to complete.
    jsTest.log("Waiting for migration to complete");
    pauseTenantMigrationBeforeLeavingDataSyncState.off();
    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    // Print the no-op oplog entries for debugging purposes.
    jsTestLog("Recipient oplog migration entries.");
    printjson(recipientPrimary.getDB("local")
                  .oplog.rs.find({op: 'n', fromTenantMigration: {$exists: true}})
                  .sort({'$natural': -1})
                  .toArray());

    function testRecipientRetryableWrites(db, writes) {
        const kCollName = writes.collName;
        jsTestLog("Testing retryable inserts");
        assert.commandWorked(db.runCommand(writes.retryableInsertCommand));
        // If retryable inserts don't work, we will see 6 here.
        assert.eq(3, db[kCollName].find({tag: writes.insertTag}).itcount());
        assert.eq(3, db[kCollName].find({tag: writes.updateTag}).itcount());
    }
    jsTestLog("Run retryable write on primary after the migration");
    testRecipientRetryableWrites(recipientDb, beforeWrites);
    testRecipientRetryableWrites(recipientDb, duringWrites);

    jsTestLog("Step up secondary");
    const recipientRst = tenantMigrationTest.getRecipientRst();
    recipientRst.stepUp(recipientRst.getSecondary());
    jsTestLog("Run retryable write on secondary after the migration");
    testRecipientRetryableWrites(recipientRst.getPrimary().getDB(kDbName), beforeWrites);
    testRecipientRetryableWrites(recipientRst.getPrimary().getDB(kDbName), duringWrites);

    tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);

    jsTestLog("Trying a back-to-back migration");
    const tenantMigrationTest2 = new TenantMigrationTest(
        {name: jsTestName() + "2", donorRst: tenantMigrationTest.getRecipientRst()});
    const recipient2Primary = tenantMigrationTest2.getRecipientPrimary();
    const recipient2Db = recipient2Primary.getDB(kDbName);
    const migrationOpts2 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: kTenantId,
    };

    TenantMigrationTest.assertCommitted(tenantMigrationTest2.runMigration(migrationOpts2));

    // Print the no-op oplog entries for debugging purposes.
    jsTestLog("Second recipient oplog migration entries.");
    printjson(recipient2Primary.getDB("local")
                  .oplog.rs.find({op: 'n', fromTenantMigration: {$exists: true}})
                  .sort({'$natural': -1})
                  .toArray());

    jsTestLog("Test retryable write on primary after the second migration");
    testRecipientRetryableWrites(recipient2Db, beforeWrites);
    testRecipientRetryableWrites(recipient2Db, duringWrites);

    tenantMigrationTest2.stop();
    tenantMigrationTest.stop();
}

testRetryOnRecipient(true);
testRetryOnRecipient(false);
})();
