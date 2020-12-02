/**
 * Tests aggregation pipeline for cloning oplog chains for retryable writes on the tenant migration
 * donor that committed before a certain donor Timestamp.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/uuid_util.js");

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    nodeOptions: {
        setParameter: {
            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    }
});
const recipientRst = new ReplSetTest({nodes: 1, name: "recipient"});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    recipientRst.stopSet();
    return;
}

const kTenantId = "testTenantId";
const kDbName = kTenantId + "_" +
    "testDb";
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const donorPrimary = donorRst.getPrimary();
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

jsTest.log("Run a migration to completion");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));

const donorDoc =
    donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({tenantId: kTenantId});

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(donorRst.nodes, migrationId, kTenantId);

// Test the aggregation pipeline the recipient would use for getting the config.transactions entries
// and oplog chains for the retryable writes that committed before startFetchingTimestamp. The
// recipient would use the real startFetchingTimestamp, but this test uses the donor's commit
// timestamp as a substitute.
const startFetchingTimestamp = donorDoc.commitOrAbortOpTime.ts;

jsTest.log("Run retryable write after the migration");
const lsid7 = {
    id: UUID()
};
const sessionTag7 = "retryable insert after migration";
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    insert: kCollName,
    documents: [{_id: 7, x: 7, tag: sessionTag7}],
    txnNumber: NumberLong(0),
    lsid: lsid7
}));

// The aggregation pipeline will return an array of oplog entries (pre-image/post-image oplog
// entries included) for retryable writes that committed before 'startFetchingTimestamp' sorted
// in ascending order of "ts". The pipeline doesn't currently support retryable write oplog chains
// that exceed 100 MB since pipeline stages have a memory limit of 100 MB.
const aggRes = donorPrimary.getDB("config").runCommand({
    aggregate: "transactions",
    pipeline: [
        // Fetch the config.transactions entries.
        {$match: {"lastWriteOpTime.ts": {$lt: startFetchingTimestamp}}},
        // Fetch latest oplog entry for each config.transactions entry.
        {$lookup: {
            from: {db: "local", coll: "oplog.rs"},
            localField: "lastWriteOpTime.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element.
            as: "lastOps"
        }},
        // Replace the single-element 'lastOps' array field with a single 'lastOp' field.
        {$addFields: {lastOp: {$first: "$lastOps"}}},
        {$unset: "lastOps"},
        // Fetch preImage oplog entry for findAndModify.
        {$lookup: {
            from: {db: "local", coll: "oplog.rs"},
            localField: "lastOp.preImageOpTime.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element if the 'preImageOpTime'
            // field is not null.
            as: "preImageOps"
        }},
        // Fetch postImage oplog entry for findAndModify.
        {$lookup: {
            from: {db: "local", coll: "oplog.rs"},
            localField: "lastOp.postImageOpTime.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element if the 'postImageOpTime'
            // field is not null.
            as: "postImageOps"
        }},
        // Fetch oplog entries in each chain for insert, update, or delete.
        {$graphLookup: {
            from: {db: "local", coll: "oplog.rs"},
            startWith: "$lastOp.ts",
            connectFromField: "prevOpTime.ts",
            connectToField: "ts",
            as: "history",
        }},
        // Combine the oplog entries.
        {$set: {history: {$concatArrays: ["$preImageOps", "$history", "$postImageOps"]}}},
        // Unwind oplog entries in each chain to the top-level array.
        {$unwind: "$history"},
        {$replaceRoot: {newRoot: "$history"}},
        // Sort the oplog entries.
        {$sort: {ts: 1}},
    ],
    readConcern: {level: "majority", afterClusterTime: startFetchingTimestamp},
    cursor: {},
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
//     "ns" : "testTenantId_testDb.testColl",
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
//     "ns" : "testTenantId_testDb.testColl",
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

// Verify that the aggregation command returned the expected number of oplog entries.
assert.eq(
    aggRes.cursor.firstBatch.length,
    sessionsOnDonor.reduce(
        (numOplogEntries, sessionOnDonor) => sessionOnDonor.numOplogEntries + numOplogEntries, 0));

// Verify that the oplog docs are sorted in ascending order of "ts".
for (let i = 1; i < aggRes.cursor.firstBatch.length; i++) {
    assert.lt(0, bsonWoCompare(aggRes.cursor.firstBatch[i].ts, aggRes.cursor.firstBatch[i - 1].ts));
}

for (let sessionOnDonor of sessionsOnDonor) {
    // Find the returned oplog docs for the session.
    const docs = aggRes.cursor.firstBatch.filter(
        doc => bsonWoCompare(doc.lsid, sessionOnDonor.txnEntry._id) === 0);
    assert.eq(docs.length, sessionOnDonor.numOplogEntries);

    docs.forEach(doc => {
        // Verify the doc corresponds to the right config.transactions entry.
        assert.eq(doc.txnNumber, sessionOnDonor.txnEntry.txnNum);

        // Verify that doc contains the right oplog entry.
        if (sessionOnDonor.tag && doc.op != "n") {
            assert.eq(getTagFromOplog(doc), sessionOnDonor.tag);
        }
    });
}

donorRst.stopSet();
recipientRst.stopSet();
})();
