/**
 * Tests aggregation pipeline for cloning oplog chains for retryable writes on the tenant migration
 * donor that committed before a certain donor Timestamp.
 *
 * Relies on MT Migrations implementation details, overall end-to-end behavior of migrating
 * retryable writes is tested elsewhere.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   incompatible_with_shard_merge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeTenantDB,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

const migrationX509Options = makeX509OptionsForTest();

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    serverless: true,
    nodeOptions: Object.assign(migrationX509Options.donor, {
        setParameter: {
            // Allow non-timestamped reads on donor after migration completes for testing.
            'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
        }
    })
});
donorRst.startSet();
donorRst.initiate();

if (isShardMergeEnabled(donorRst.getPrimary().getDB("admin"))) {
    donorRst.stopSet();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

// TODO(SERVER-86809): Re-enable this test.
if (FeatureFlagUtil.isPresentAndEnabled(donorRst.getPrimary(),
                                        "ReplicateVectoredInsertsTransactionally")) {
    donorRst.stopSet();
    jsTestLog(
        "Retryable write migration test temporarily disabled with feature ReplicateVectoredInsertsTransactionally.");
    quit();
}

const recipientRst = new ReplSetTest(
    {nodes: 1, name: "recipient", serverless: true, nodeOptions: migrationX509Options.recipient});
recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

const kTenantId = ObjectId().str;
const kDbName = makeTenantDB(kTenantId, "testDb");
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();
const configTxnColl = donorPrimary.getCollection("config.transactions");

assert.commandWorked(donorPrimary.getCollection(kNs).insert(
    [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}], {writeConcern: {w: "majority"}}));

function getTxnEntry(lsid) {
    return configTxnColl.findOne({"_id.id": lsid.id});
}

// Each retryable insert and update below is identified by a unique 'tag'. This function returns the
// value of the 'tag' field inside the 'o' field of the given 'oplogEntry'.
function getTagFromOplog(oplogEntry) {
    if (oplogEntry.op == "i" || oplogEntry.op == "d") {
        return oplogEntry.o.tag;
    }
    if (oplogEntry.op == "u") {
        return oplogEntry.o.$v === 1 ? oplogEntry.o.$set.tag : oplogEntry.o.diff.u.tag;
    }
    throw Error("Unknown op type " + oplogEntry.op);
}

let sessionsOnDonor = [];

jsTest.log("Run retryable writes prior to the migration");

// Test batched inserts.
const lsid1 = {
    id: UUID()
};
const sessionTag1 = "retryable insert";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{x: 3, tag: sessionTag1}, {x: 4, tag: sessionTag1}, {x: 5, tag: sessionTag1}],
    txnNumber: NumberLong(0),
    lsid: lsid1
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid1),
    numOplogEntries: 3,
    tag: sessionTag1,
});

// Test batched updates.
const lsid2 = {
    id: UUID()
};
const sessionTag2 = "retryable update";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    update: kCollName,
    updates: [
        {q: {x: 3}, u: {$set: {tag: sessionTag2}}},
        {q: {x: 4}, u: {$set: {tag: sessionTag2}}},
        {q: {x: 5}, u: {$set: {tag: sessionTag2}}}
    ],
    txnNumber: NumberLong(0),
    lsid: lsid2
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid2),
    numOplogEntries: 3,
    tag: sessionTag2,
});

// Test batched deletes.
const lsid3 = {
    id: UUID()
};
// Use limit: 1 because multi-deletes are not supported in retryable writes.
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    delete: kCollName,
    deletes: [{q: {x: 3}, limit: 1}, {q: {x: 4}, limit: 1}, {q: {x: 5}, limit: 1}],
    txnNumber: NumberLong(0),
    lsid: lsid3
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid3),
    numOplogEntries: 3,
});

// Test findAndModify oplog entry without preImageOpTime or postImageOpTime.
const lsid4 = {
    id: UUID()
};
const sessionTag4 = "retryable findAndModify upsert";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    findAndModify: kCollName,
    query: {x: 6},
    update: {x: 6, tag: sessionTag4},
    upsert: true,
    txnNumber: NumberLong(0),
    lsid: lsid4
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid4),
    numOplogEntries: 1,
    tag: sessionTag4,
});

// Test findAndModify oplog entry with postImageOpTime.
const lsid5 = {
    id: UUID()
};
const sessionTag5 = "retryable findAndModify update";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    findAndModify: kCollName,
    query: {x: 6},
    update: {$set: {tag: sessionTag5}},
    new: true,
    txnNumber: NumberLong(0),
    lsid: lsid5
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid5),
    containsPostImage: true,
    numOplogEntries: 2,  // one post-image oplog entry.
    tag: sessionTag5
});

// Test findAndModify oplog entry with preImageOpTime.
const lsid6 = {
    id: UUID()
};
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    findAndModify: kCollName,
    query: {x: 6},
    remove: true,
    txnNumber: NumberLong(0),
    lsid: lsid6
}));
sessionsOnDonor.push({
    txnEntry: getTxnEntry(lsid6),
    containsPreImage: true,
    numOplogEntries: 2,  // one pre-image oplog entry.
});

// Example oplog entries output for the retryable findAndModify in session 'lsid6' where the first
// one is its pre-image oplog entry.
// {
//     "lsid" : {
//         "id" : UUID("99e24c9c-3da0-48dc-9b31-ab72460e666c"),
//         "uid" : BinData(0,"47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=")
//     },
//     "txnNumber" : NumberLong(0),
//     "op" : "n",
//     "ns" : "<OID>_testDb.testColl",
//     "ui" : UUID("1aa099b9-879f-4cd5-b58e-0a654abfeb58"),
//     "o" : {
//         "_id" : ObjectId("5fa4d04d04c649017b6558ff"),
//         "x" : 6,
//         "tag" : "retryable findAndModify update"
//     },
//     "ts" : Timestamp(1604636749, 17),
//     "t" : NumberLong(1),
//     "wall" : ISODate("2020-11-06T04:25:49.765Z"),
//     "v" : NumberLong(2),
//     "stmtId" : 0,
//     "prevOpTime" : {
//         "ts" : Timestamp(0, 0),
//         "t" : NumberLong(-1)
//     }
// },
// {
//     "lsid" : {
//         "id" : UUID("99e24c9c-3da0-48dc-9b31-ab72460e666c"),
//         "uid" : BinData(0,"47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=")
//     },
//     "txnNumber" : NumberLong(0),
//     "op" : "d",
//     "ns" : "<OID>_testDb.testColl",
//     "ui" : UUID("1aa099b9-879f-4cd5-b58e-0a654abfeb58"),
//     "o" : {
//         "_id" : ObjectId("5fa4d04d04c649017b6558ff")
//     },
//     "preImageOpTime" : {
//         "ts" : Timestamp(1604636749, 17),
//         "t" : NumberLong(1)
//     },
//     "ts" : Timestamp(1604636749, 18),
//     "t" : NumberLong(1),
//     "wall" : ISODate("2020-11-06T04:25:49.765Z"),
//     "v" : NumberLong(2),
//     "stmtId" : 0,
//     "prevOpTime" : {
//         "ts" : Timestamp(0, 0),
//         "t" : NumberLong(-1)
//     }
// }

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

const fpAfterRetrievingStartOpTimesMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterRetrievingStartOpTimesMigrationRecipientInstance", {action: "hang"});
const fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime = configureFailPoint(
    recipientPrimary, "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime", {action: "hang"});
const fpAfterDataConsistentMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterDataConsistentMigrationRecipientInstance", {action: "hang"});

jsTestLog(`Starting tenant migration: ${tojson(migrationOpts)}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for recipient to get the startFetchingTimestamp.
fpAfterRetrievingStartOpTimesMigrationRecipientInstance.wait();

// Do retryable writes after retrieving startFetchingTimestamp, these writes should not appear in
// the oplog buffer in the pre-fetch stage, but should exit after tenant migration is consistent.
const lsid7 = {
    id: UUID()
};
const sessionTag7 = "retryable insert after retrieving startFetchingTimestamp";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{_id: 7, x: 7, tag: sessionTag7}],
    txnNumber: NumberLong(0),
    lsid: lsid7
}));

// Wait for retryable writes to be fetched and inserted into oplog buffer prior to cloning.
fpAfterRetrievingStartOpTimesMigrationRecipientInstance.off();
fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime.wait();

const kOplogBufferNS = "config.repl.migration.oplog_" + migrationOpts.migrationIdString;
const recipientOplogBuffer = recipientPrimary.getCollection(kOplogBufferNS);
jsTestLog(`oplog buffer ns: ${kOplogBufferNS}`);

// Verify that after pre-fetching retryable writes, the entries inserted into the oplog buffer
// are equal to the entries on the donor.
const findRes = recipientOplogBuffer.find().toArray();
const expectedCount = sessionsOnDonor.reduce(
    (numOplogEntries, sessionOnDonor) => sessionOnDonor.numOplogEntries + numOplogEntries, 0);
assert.eq(
    findRes.length, expectedCount, `Incorrect number of oplog buffer entries: ${tojson(findRes)}`);

for (const session of sessionsOnDonor) {
    // Find the returned oplog docs for the session.
    const docs = recipientOplogBuffer.find({"entry.lsid": session.txnEntry._id}).toArray();
    assert.eq(docs.length, session.numOplogEntries);

    docs.forEach(doc => {
        // Verify the doc corresponds to the right config.transactions entry.
        assert.eq(doc.entry.txnNumber, session.txnEntry.txnNum);
        // Verify that doc contains the right oplog entry.
        if (doc.entry.op === "n") {
            assert.eq(session.containsPreImage || session.containsPostImage, true);
        } else if (session.tag) {
            assert.eq(getTagFromOplog(doc.entry), session.tag);
        }
    });
}

// Wait for tenant migration to be consistent.
fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime.off();
fpAfterDataConsistentMigrationRecipientInstance.wait();

// After tenant migration is consistent, the retryable writes done after startFetchingTimestamp
// should have been fetched and inserted into the oplog buffer.
const findRes2 = recipientOplogBuffer.find().toArray();
const expectedCount2 = expectedCount + 1;
assert.eq(findRes2.length, expectedCount2);

const docs2 = recipientOplogBuffer.find({"entry.lsid": getTxnEntry(lsid7)._id}).toArray();
assert.eq(docs2.length, 1);
assert.eq(getTagFromOplog(docs2[0].entry), sessionTag7);

// Wait for tenant migration to complete successfully.
fpAfterDataConsistentMigrationRecipientInstance.off();
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

donorRst.stopSet();
recipientRst.stopSet();
