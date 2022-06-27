/**
 * Tests that retryable writes made on the donor during a tenant migration can be properly retried
 * on the recipient.
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

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDb");
const kCollNameBefore = "testCollBefore";
const kCollNameDuring = "testCollDuring";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const donorDb = donorPrimary.getDB(kDbName);
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const recipientDb = recipientPrimary.getDB(kDbName);

// TODO (SERVER-61677): Currently, when we call replSetStepUp below, the new recipient secondary
// wrongly restarts the Shard Merge protocol. It copies and imports donor files again, and
// eventually hits an invariant in TenantFileImporterService, which doesn't support restart.
// Once we fix Shard Merge to not resume on stepup, this test will work as-is.
if (TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("adminDB"))) {
    jsTestLog("Skip: featureFlagShardMerge enabled, but shard merge does not survive stepup");
    tenantMigrationTest.stop();
    return;
}

jsTestLog("Run a migration to the end of cloning");
const waitBeforeFetchingTransactions =
    configureFailPoint(recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};
function setupRetryableWritesForCollection(collName) {
    const kNs = `${kDbName}.${collName}`;
    assert.commandWorked(donorPrimary.getCollection(kNs).insert([{x: 0}, {x: 1}, {x: 2}],
                                                                {writeConcern: {w: "majority"}}));
    // For the updates.
    assert.commandWorked(donorPrimary.getCollection(kNs).insert([{x: 6}, {x: 7}, {x: 8}],
                                                                {writeConcern: {w: "majority"}}));
    // For the deletes.
    assert.commandWorked(donorPrimary.getCollection(kNs).insert(
        [{x: 9, tag: "delete"}, {x: 10, tag: "delete"}, {x: 11, tag: "delete"}],
        {writeConcern: {w: "majority"}}));

    let result = {collName: collName};
    // For the find-and-modify.  Note there is no "x: 12"; "x: 12" will be upserted.
    const kTagFindAndModifyBefore = "find and modify before";
    const kFindAndModifyExtra = "extra";
    result.findAndModifyExtra = kFindAndModifyExtra;
    assert.commandWorked(donorPrimary.getCollection(kNs).insert(
        [
            {x: 13, tag: kTagFindAndModifyBefore, extra: kFindAndModifyExtra},
            {x: 14, tag: kTagFindAndModifyBefore}
        ],
        {writeConcern: {w: "majority"}}));

    // Setup command for batched inserts.
    const lsid1 = {id: UUID()};
    const sessionTag1 = "retryable insert " + collName;
    result.sessionTag1 = sessionTag1;
    result.retryableInsertCommand = {
        insert: collName,
        documents: [{x: 3, tag: sessionTag1}, {x: 4, tag: sessionTag1}, {x: 5, tag: sessionTag1}],
        txnNumber: NumberLong(0),
        lsid: lsid1
    };

    // Setup command for batched updates.
    const lsid2 = {id: UUID()};
    const sessionTag2 = "retryable update " + collName;
    result.sessionTag2 = sessionTag2;
    result.retryableUpdateCommand = {
        update: collName,
        updates: [
            {q: {x: 6}, u: {$set: {tag: sessionTag2}}},
            {q: {x: 7}, u: {$set: {tag: sessionTag2}}},
            {q: {x: 8}, u: {$set: {tag: sessionTag2}}}
        ],
        txnNumber: NumberLong(0),
        lsid: lsid2
    };

    // Setup batched deletes.
    const lsid3 = {id: UUID()};
    // Use limit: 1 because multi-deletes are not supported in retryable writes.
    result.retryableDeleteCommand = {
        delete: collName,
        deletes: [{q: {x: 9}, limit: 1}, {q: {x: 10}, limit: 1}, {q: {x: 11}, limit: 1}],
        txnNumber: NumberLong(0),
        lsid: lsid3
    };

    // Setup findAndModify oplog entry without preImageOpTime or postImageOpTime.
    const lsid4 = {id: UUID()};
    const sessionTag4 = "retryable findAndModify upsert " + collName;
    result.sessionTag4 = sessionTag4;
    result.retryableFindAndModifyUpsertCommand = {
        findAndModify: collName,
        query: {x: 12},
        update: {x: 12, tag: sessionTag4},
        upsert: true,
        txnNumber: NumberLong(0),
        lsid: lsid4
    };

    // Setup findAndModify oplog entry with postImageOpTime.
    const lsid5 = {id: UUID()};
    const sessionTag5 = "retryable findAndModify update " + collName;
    result.sessionTag5 = "retryable findAndModify update " + collName;
    result.retryableFindAndModifyUpdateWithPostImageCommand = {
        findAndModify: collName,
        query: {x: 13},
        update: {$set: {tag: sessionTag5}},
        new: true,
        txnNumber: NumberLong(0),
        lsid: lsid5
    };

    // Setup findAndModify oplog entry with preImageOpTime.
    const lsid6 = {id: UUID()};
    result.retryableFindAndModifyUpdateWithPreImageCommand = {
        findAndModify: collName,
        query: {x: 14},
        remove: true,
        txnNumber: NumberLong(0),
        lsid: lsid6
    };
    return result;
}

const beforeWrites = setupRetryableWritesForCollection(kCollNameBefore);
const duringWrites = setupRetryableWritesForCollection(kCollNameDuring);

jsTestLog("Run retryable writes before the migration");

assert.commandWorked(donorDb.runCommand(beforeWrites.retryableInsertCommand));
assert.commandWorked(donorDb.runCommand(beforeWrites.retryableUpdateCommand));
assert.commandWorked(donorDb.runCommand(beforeWrites.retryableDeleteCommand));
assert.commandWorked(donorDb.runCommand(beforeWrites.retryableFindAndModifyUpsertCommand));
assert.commandWorked(
    donorDb.runCommand(beforeWrites.retryableFindAndModifyUpdateWithPostImageCommand));
assert.commandWorked(
    donorDb.runCommand(beforeWrites.retryableFindAndModifyUpdateWithPreImageCommand));

const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
waitBeforeFetchingTransactions.wait();

jsTestLog("Run retryable writes during the migration");

assert.commandWorked(donorDb.runCommand(duringWrites.retryableInsertCommand));
assert.commandWorked(donorDb.runCommand(duringWrites.retryableUpdateCommand));
assert.commandWorked(donorDb.runCommand(duringWrites.retryableDeleteCommand));
assert.commandWorked(donorDb.runCommand(duringWrites.retryableFindAndModifyUpsertCommand));
assert.commandWorked(
    donorDb.runCommand(duringWrites.retryableFindAndModifyUpdateWithPostImageCommand));
assert.commandWorked(
    donorDb.runCommand(duringWrites.retryableFindAndModifyUpdateWithPreImageCommand));

// Wait for the migration to complete.
jsTest.log("Waiting for migration to complete");
waitBeforeFetchingTransactions.off();
TenantMigrationTest.assertCommitted(migrationThread.returnData());

// Print the no-op oplog entries for debugging purposes.
jsTestLog("Recipient oplog migration entries.");
printjson(recipientPrimary.getDB("local")
              .oplog.rs.find({op: 'n', fromTenantMigration: {$exists: true}})
              .sort({'$natural': -1})
              .toArray());

const kNoTag = "no tag";
function modifyDataForErrorDetection(writes) {
    const kCollName = writes.collName;
    // Changed the updated items so we can be assured the update did not run again.
    assert.eq(3, recipientDb[kCollName].find({tag: writes.sessionTag2}).itcount());
    assert.commandWorked(recipientDb.runCommand({
        update: kCollName,
        updates: [{q: {tag: writes.sessionTag2}, u: {$set: {tag: kNoTag}}, multi: true}],
    }));

    // Change the docs modified by findAndModify so we can sure they are not updated on a re-run.
    assert.eq(1, recipientDb[kCollName].find({tag: writes.sessionTag4}).itcount());
    assert.commandWorked(recipientDb.runCommand({
        update: kCollName,
        updates: [
            {q: {tag: writes.sessionTag4}, u: {$set: {tag: kNoTag}}},
            {q: {tag: writes.sessionTag5}, u: {$set: {tag: kNoTag, extra: "none"}}}
        ],
    }));

    // Add an item which matches the deleted-item query so we know the delete doesn't run again.
    assert.commandWorked(recipientDb[kCollName].insert({x: 9, tag: kNoTag}));
}
modifyDataForErrorDetection(beforeWrites);
modifyDataForErrorDetection(duringWrites);

function testRecipientRetryableWrites(db, writes) {
    const kCollName = writes.collName;
    jsTestLog("Testing retryable inserts");
    assert.commandWorked(db.runCommand(writes.retryableInsertCommand));
    // If retryable inserts don't work, we will see 6 here.
    assert.eq(3, db[kCollName].find({tag: writes.sessionTag1}).itcount());

    jsTestLog("Testing retryable update");
    assert.commandWorked(db.runCommand(writes.retryableUpdateCommand));
    // If retryable updates don't work, we will see 3 here.
    assert.eq(0, db[kCollName].find({tag: writes.sessionTag2}).itcount());

    jsTestLog("Testing retryable delete");
    assert.commandWorked(db.runCommand(writes.retryableDeleteCommand));
    // If retryable deletes don't work, we will see 0 here.
    assert.eq(1, db[kCollName].find({x: 9}).itcount());

    jsTestLog("Testing retryable findAndModify with upsert");
    assert.commandWorked(db.runCommand(writes.retryableFindAndModifyUpsertCommand));
    const findAndModifyUpsertDoc = db[kCollName].findOne({x: 12});
    // If the retryable find and modify is erroneously re-run, we will see sessionTag4 here
    assert.eq(kNoTag, findAndModifyUpsertDoc.tag);

    jsTestLog("Testing retryable findAndModify with update and postImage");
    let res = assert.commandWorked(
        db.runCommand(writes.retryableFindAndModifyUpdateWithPostImageCommand));
    // If postimages don't work right, we'll see "none" for extra or a null here.
    assert(res.value);
    assert.eq(res.value.extra, writes.findAndModifyExtra);
    const findAndModifyUpdatePostImageDoc = db[kCollName].findOne({x: 13});
    // If the retryable find and modify is erroneously re-run, we will see sessionTag5 here
    assert.eq(kNoTag, findAndModifyUpdatePostImageDoc.tag);

    jsTestLog("Testing retryable findAndModify with delete and preImage");
    res =
        assert.commandWorked(db.runCommand(writes.retryableFindAndModifyUpdateWithPreImageCommand));
    // If preimages don't work right, we'll see a null here.
    assert(res.value);
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
})();
