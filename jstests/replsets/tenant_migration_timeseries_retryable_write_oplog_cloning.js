/**
 * Tests aggregation pipeline for cloning oplog chains for retryable writes on the tenant migration
 * donor that committed before a certain donor Timestamp for time-series collections oplog entries
 * with multiple statement IDs.
 *
 * This test is based on "tenant_migration_retryable_write_retry.js".
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
load("jstests/libs/uuid_util.js");

function testOplogCloning(ordered) {
    const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
    const kGarbageCollectionParams = {
        // Set the delay before a donor state doc is garbage collected to be short to speed up
        // the test.
        tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

        // Set the TTL monitor to run at a smaller interval to speed up the test.
        ttlMonitorSleepSecs: 1,
    };

    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donor",
        nodeOptions:
            Object.assign(migrationX509Options.donor, {setParameter: kGarbageCollectionParams})
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipient",
        nodeOptions:
            Object.assign(migrationX509Options.recipient, {setParameter: kGarbageCollectionParams})
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

    const donorPrimary = donorRst.getPrimary();

    const kTenantId = "testTenantId";
    const kDbName = kTenantId + "_" +
        "tsDb";
    const kCollName = "tsColl";
    const kNs = `${kDbName}.${kCollName}`;

    const tsDB = donorPrimary.getDB(kDbName);
    assert.commandWorked(
        tsDB.createCollection(kCollName, {timeseries: {timeField: "time", metaField: "meta"}}));
    const tag1 = "regular insert";
    assert.commandWorked(donorPrimary.getCollection(kNs).insert(
        [
            {_id: 0, time: ISODate(), x: 0, tag: tag1, meta: 0},
            {_id: 1, time: ISODate(), x: 1, tag: tag1, meta: 0},
            {_id: 2, time: ISODate(), x: 2, tag: tag1, meta: 0},
        ],
        {writeConcern: {w: "majority"}}));

    // Each retryable insert and update below is identified by a unique 'tag'. This function returns
    // the value of the 'tag' field inside the 'o' field of the given 'oplogEntry'.
    function getTagsFromOplog(oplogEntry) {
        if (oplogEntry.op == "i") {
            return Object.values(oplogEntry.o.data.tag);
        }
        if (oplogEntry.op == "u") {
            return Object.values(oplogEntry.o.diff.sdata.stag.i);
        }
        throw Error("Unknown op type " + oplogEntry.op);
    }

    jsTest.log("Run retryable writes prior to the migration");

    const lsid1 = {id: UUID()};
    const insertTag = "retryable insert";
    const updateTag = "retryable update";
    assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [
            // Test batched inserts resulting in "insert" oplog entries.
            {time: ISODate(), x: 0, tag: insertTag, meta: 1},
            {time: ISODate(), x: 1, tag: insertTag, meta: 1},
            {time: ISODate(), x: 2, tag: insertTag, meta: 1},
            // Test batched inserts resulting in "update" oplog entries.
            {time: ISODate(), x: 3, tag: updateTag, meta: 0},
            {time: ISODate(), x: 4, tag: updateTag, meta: 0},
            {time: ISODate(), x: 5, tag: updateTag, meta: 0},
        ],
        txnNumber: NumberLong(0),
        lsid: lsid1,
        ordered: ordered,
    }));

    jsTest.log("Run a migration to completion");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: kTenantId
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);

    // Test the aggregation pipeline the recipient would use for getting the oplog chain where
    // "ts" < "startFetchingOpTime" for all retryable writes entries in config.transactions. The
    // recipient would use the real "startFetchingOpTime", but this test uses the donor's commit
    // timestamp as a substitute.
    const startFetchingTimestamp = donorDoc.commitOrAbortOpTime.ts;

    jsTest.log("Run retryable write after the migration");
    const lsid2 = {id: UUID()};
    const sessionTag2 = "retryable insert after migration";
    // Make sure this write is in the majority snapshot.
    assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{_id: 6, time: ISODate(), x: 6, tag: sessionTag2}],
        txnNumber: NumberLong(0),
        lsid: lsid2,
        writeConcern: {w: "majority"},
    }));

    // The aggregation pipeline will return an array of retryable writes oplog entries (pre-image/
    // post-image oplog entries included) with "ts" < "startFetchingTimestamp" and sorted in
    // ascending order of "ts".
    const aggRes = donorPrimary.getDB("config").runCommand({
    aggregate: "transactions",
    pipeline: [
        // Fetch the config.transactions entries that do not have a "state" field, which indicates a
        // retryable write.
        {$match: {"state": {$exists: false}}},
        // Fetch latest oplog entry for each config.transactions entry from the oplog view.
        {$lookup: {
            from: {db: "local", coll: "system.tenantMigration.oplogView"},
            let: { tenant_ts: "$lastWriteOpTime.ts"},
            pipeline: [{
                $match: {
                    $expr: {
                        $and: [
                            {$regexMatch: {
                                input: "$ns",
                                regex: new RegExp(`^${kTenantId}_`)
                            }},
                            {$eq: [ "$ts", "$$tenant_ts"]}
                        ]
                    }
                }
            }],
            // This array is expected to contain exactly one element if `ns` contains
            // `kTenantId`. Otherwise, it will be empty.
            as: "lastOps"
        }},
        // Entries that don't have the correct `ns` will return an empty `lastOps` array. Filter
        // these results before the next stage.
        {$match: {"lastOps": {$ne: [] }}},
        // All remaining results should correspond to the correct `kTenantId`. Replace the
        // single-element 'lastOps' array field with a single 'lastOp' field.
        {$addFields: {lastOp: {$first: "$lastOps"}}},
        {$unset: "lastOps"},
        // Fetch preImage oplog entry for findAndModify from the oplog view.
        {$lookup: {
            from: {db: "local", coll: "system.tenantMigration.oplogView"},
            localField: "lastOp.preImageOpTime.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element if the 'preImageOpTime'
            // field is not null.
            as: "preImageOps"
        }},
        // Fetch postImage oplog entry for findAndModify from the oplog view.
        {$lookup: {
            from: {db: "local", coll: "system.tenantMigration.oplogView"},
            localField: "lastOp.postImageOpTime.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element if the 'postImageOpTime'
            // field is not null.
            as: "postImageOps"
        }},
        // Fetch oplog entries in each chain for insert, update, or delete from the oplog view.
        {$graphLookup: {
            from: {db: "local", coll: "system.tenantMigration.oplogView"},
            startWith: "$lastOp.ts",
            connectFromField: "prevOpTime.ts",
            connectToField: "ts",
            as: "history",
            depthField: "depthForTenantMigration"
        }},
        // Now that we have the whole chain, filter out entries that occurred after
        // `startFetchingTimestamp`, since these entries will be fetched during the oplog fetching
        // phase.
        {$set: {
            history: {
                $filter: {
                    input: "$history",
                    cond: {$lt: ["$$this.ts", startFetchingTimestamp]}
                }
            }
        }},
        // Sort the oplog entries in each oplog chain.
        {$set: {
            history: {$reverseArray: {$reduce: {
                input: "$history",
                initialValue: {$range: [0, {$size: "$history"}]},
                in: {$concatArrays: [
                    {$slice: ["$$value", "$$this.depthForTenantMigration"]},
                    ["$$this"],
                    {$slice: [
                        "$$value",
                        {$subtract: [
                            {$add: ["$$this.depthForTenantMigration", 1]},
                            {$size: "$history"},
                        ]},
                    ]},
                ]},
            }}},
        }},
        // Combine the oplog entries.
        {$set: {history: {$concatArrays: ["$preImageOps", "$history", "$postImageOps"]}}},
        // Fetch the complete oplog entries and unwind oplog entries in each chain to the top-level
        // array.
        {$lookup: {
            from: {db: "local", coll: "oplog.rs"},
            localField: "history.ts",
            foreignField: "ts",
            // This array is expected to contain exactly one element.
            as: "completeOplogEntry"
        }},
        // Unwind oplog entries in each chain to the top-level array.
        {$unwind: "$completeOplogEntry"},
        {$replaceRoot: {newRoot: "$completeOplogEntry"}},
    ],
    readConcern: {level: "majority"},
    cursor: {},
});

    // Verify that the aggregation command returned the expected number of oplog entries.
    assert.eq(aggRes.cursor.firstBatch.length, 2);

    // Verify that the oplog docs are sorted in ascending order of "ts".
    for (let i = 1; i < aggRes.cursor.firstBatch.length; i++) {
        assert.lt(
            0, bsonWoCompare(aggRes.cursor.firstBatch[i].ts, aggRes.cursor.firstBatch[i - 1].ts));
    }

    const docs = aggRes.cursor.firstBatch;
    // Verify the number of statement ids is correct.
    assert.eq(docs[0].stmtId.length, 3);
    assert.eq(docs[1].stmtId.length, 3);

    // Verify that docs contain the right oplog entry.
    getTagsFromOplog(docs[0]).forEach(tag => {
        assert.eq(tag, insertTag);
    });
    getTagsFromOplog(docs[1]).forEach(tag => {
        assert.eq(tag, updateTag);
    });

    donorRst.stopSet();
    recipientRst.stopSet();
}

testOplogCloning(true);
testOplogCloning(false);
})();
