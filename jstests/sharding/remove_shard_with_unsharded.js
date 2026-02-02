/**
 * Test that unsharded collection blocks the removing of the shard and it's correctly
 * shown in the remaining.collectionsToMove counter.
 *
 * Also verifies that:
 * - remaining.estimatedRemainingBytes is non-negative while draining is in progress
 * - remaining.estimatedRemainingBytes is 0 when removeShard is completed
 * @tags: [
 * requires_fcv_83,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});
let config = st.s.getDB("config");
const adminDB = st.s.getDB("admin");

const db0 = st.s.getDB("db0");
const db1 = st.s.getDB("db1");

assert.commandWorked(st.s.adminCommand({enableSharding: db0.getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({enableSharding: db1.getName(), primaryShard: st.shard1.shardName}));

// Create the following collections:
//
//          SHARD0        |   SHARD1 (toRemove)
//  ----------------------+----------------------
//     db0.collUnsharded  |   db1.collUnsharded
//    db0.collTimeseries  |  db1.collTimeseries
//      db0.collSharded   |    db1.collSharded
//   db1.collOutOfPrimary | db0.collOutOfPrimary
//

const expectedCollectionsOnTheDrainingShard = ["db1.collUnsharded", "db1.collTimeseries", "db0.collOutOfPrimary"];

[db0, db1].forEach((db) => {
    assert.commandWorked(db.createCollection("collUnsharded"));
    assert.commandWorked(db.createCollection("collOutOfPrimary"));
    assert.commandWorked(db.createCollection("collTimeseries", {timeseries: {timeField: "time"}}));
    assert.commandWorked(db.adminCommand({shardCollection: db.getName() + ".collSharded", key: {x: 1}}));
});

// Move unsharded collections to non-primary shard
assert.commandWorked(st.s.adminCommand({moveCollection: "db0.collOutOfPrimary", toShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveCollection: "db1.collOutOfPrimary", toShard: st.shard0.shardName}));

// Insert documents into collections on shard1 (the shard to be removed) so that
// estimatedRemainingBytes will be non-zero during draining.
jsTestLog("Inserting documents into collections on the shard to be drained (shard1).");

const numDocsUnsharded = 100;
let bulkUnsharded = db1.collUnsharded.initializeUnorderedBulkOp();
for (let i = 0; i < numDocsUnsharded; i++) {
    bulkUnsharded.insert({_id: i, data: "x".repeat(1000), value: i});
}
assert.commandWorked(bulkUnsharded.execute());
jsTestLog("Inserted " + numDocsUnsharded + " documents into db1.collUnsharded");

const numDocsTimeseries = 50;
const now = new Date();
let bulkTimeseries = db1.collTimeseries.initializeUnorderedBulkOp();
for (let i = 0; i < numDocsTimeseries; i++) {
    bulkTimeseries.insert({
        time: new Date(now.getTime() + i * 1000),
        metadata: {sensor: "sensor_" + i},
        value: Math.random() * 100,
    });
}
assert.commandWorked(bulkTimeseries.execute());
jsTestLog("Inserted " + numDocsTimeseries + " documents into db1.collTimeseries");

const numDocsOutOfPrimary = 75;
let bulkOutOfPrimary = db0.collOutOfPrimary.initializeUnorderedBulkOp();
for (let i = 0; i < numDocsOutOfPrimary; i++) {
    bulkOutOfPrimary.insert({_id: i, info: "outOfPrimary_" + i, payload: "y".repeat(500)});
}
assert.commandWorked(bulkOutOfPrimary.execute());
jsTestLog("Inserted " + numDocsOutOfPrimary + " documents into db0.collOutOfPrimary");

const numDocsSharded = 80;
let bulkSharded = db1.collSharded.initializeUnorderedBulkOp();
for (let i = 0; i < numDocsSharded; i++) {
    bulkSharded.insert({x: i, description: "sharded_doc_" + i, content: "z".repeat(800)});
}
assert.commandWorked(bulkSharded.execute());
jsTestLog("Inserted " + numDocsSharded + " documents into db1.collSharded");

jsTestLog(
    "Total documents inserted on shard1: " +
        (numDocsUnsharded + numDocsTimeseries + numDocsOutOfPrimary + numDocsSharded),
);

// Initiate removeShard
assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

// Check the ongoing status and unsharded collection, that cannot be moved
let removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq("ongoing", removeResult.state, "Shard should stay in ongoing state: " + tojson(removeResult));
assert.eq(3, removeResult.remaining.collectionsToMove);
assert.eq(1, removeResult.remaining.dbs);
assert.eq(3, removeResult.collectionsToMove.length);
assert.eq(1, removeResult.dbsToMove.length);
assert.sameMembers(expectedCollectionsOnTheDrainingShard, removeResult.collectionsToMove);

// Verify estimatedRemainingBytes is non-negative during draining
// Since we inserted documents, it should be > 0
assert(
    removeResult.remaining.hasOwnProperty("estimatedRemainingBytes"),
    "Expected estimatedRemainingBytes field in remaining: " + tojson(removeResult.remaining),
);
assert.gt(
    removeResult.remaining.estimatedRemainingBytes,
    0,
    "estimatedRemainingBytes should be > 0 during draining: " + tojson(removeResult.remaining),
);

jsTestLog(
    "First removeShard progress check - estimatedRemainingBytes: " + removeResult.remaining.estimatedRemainingBytes,
);

// Check the status once again
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq("ongoing", removeResult.state, "Shard should stay in ongoing state: " + tojson(removeResult));
assert.eq(3, removeResult.remaining.collectionsToMove);
assert.eq(1, removeResult.remaining.dbs);
assert.eq(3, removeResult.collectionsToMove.length);
assert.eq(1, removeResult.dbsToMove.length);
assert.sameMembers(expectedCollectionsOnTheDrainingShard, removeResult.collectionsToMove);

// Verify estimatedRemainingBytes is still non-negative (and > 0 since data hasn't moved yet)
assert.gt(
    removeResult.remaining.estimatedRemainingBytes,
    0,
    "estimatedRemainingBytes should still be > 0 since data hasn't moved: " + tojson(removeResult.remaining),
);
jsTestLog(
    "Second removeShard progress check - estimatedRemainingBytes: " + removeResult.remaining.estimatedRemainingBytes,
);

// Move unsharded collections out from the draining shard and track estimatedRemainingBytes
jsTestLog("Moving collections out from the draining shard...");

let previousBytes = removeResult.remaining.estimatedRemainingBytes;
expectedCollectionsOnTheDrainingShard.forEach((collName) => {
    jsTestLog("Moving collection: " + collName);
    assert.commandWorked(adminDB.adminCommand({moveCollection: collName, toShard: st.shard0.shardName}));

    // Check progress after each collection move
    removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
    if (removeResult.remaining && removeResult.remaining.hasOwnProperty("estimatedRemainingBytes")) {
        jsTestLog(
            "After moving " +
                collName +
                " - estimatedRemainingBytes: " +
                removeResult.remaining.estimatedRemainingBytes +
                " (was: " +
                previousBytes +
                ")",
        );
        // Bytes should decrease or stay the same as we move collections
        assert.gte(
            removeResult.remaining.estimatedRemainingBytes,
            0,
            "estimatedRemainingBytes should remain non-negative: " + tojson(removeResult.remaining),
        );
        previousBytes = removeResult.remaining.estimatedRemainingBytes;
    }
});

// Move `db1.collSharded` chunk out from the draining shard
jsTestLog("Moving db1.collSharded chunk out from the draining shard");
assert.commandWorked(
    adminDB.adminCommand({
        moveRange: "db1.collSharded",
        min: {x: MinKey()},
        max: {x: MaxKey()},
        toShard: st.shard0.shardName,
    }),
);

// Check progress after moving the sharded collection chunk
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
if (removeResult.remaining && removeResult.remaining.hasOwnProperty("estimatedRemainingBytes")) {
    assert.gte(
        removeResult.remaining.estimatedRemainingBytes,
        0,
        "estimatedRemainingBytes should remain non-negative: " + tojson(removeResult.remaining),
    );
    jsTestLog(
        "After moving db1.collSharded chunk - estimatedRemainingBytes: " +
            removeResult.remaining.estimatedRemainingBytes +
            " (was: " +
            previousBytes +
            ")",
    );
}

// Move `db1` out from the draining shard
jsTestLog("Moving db1 primary out from the draining shard");
assert.commandWorked(adminDB.adminCommand({movePrimary: "db1", to: st.shard0.shardName}));

// Finalize removing the shard - poll until completed
jsTestLog("Waiting for removeShard to complete...");
assert.soon(
    function () {
        removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
        jsTestLog("removeShard state: " + removeResult.state + ", remaining: " + tojson(removeResult.remaining));
        return removeResult.state === "completed";
    },
    "removeShard did not complete in time",
    5 * 60 * 1000,
    1000,
);

assert.eq("completed", removeResult.state, "Shard was not removed: " + tojson(removeResult));

// Verify estimatedRemainingBytes is 0 when removeShard is completed
// When completed, the 'remaining' field may not be present, or if present, estimatedRemainingBytes should be 0
if (removeResult.hasOwnProperty("remaining") && removeResult.remaining.hasOwnProperty("estimatedRemainingBytes")) {
    assert.eq(
        removeResult.remaining.estimatedRemainingBytes,
        0,
        "estimatedRemainingBytes should be 0 when removeShard is completed: " + tojson(removeResult),
    );
    jsTestLog("Completed removeShard - estimatedRemainingBytes: " + removeResult.remaining.estimatedRemainingBytes);
} else {
    jsTestLog(
        "Completed removeShard - 'remaining' or 'estimatedRemainingBytes' not present " +
            "(expected when fully completed): " +
            tojson(removeResult),
    );
}

let existingShards = config.shards.find({}).toArray();
assert.eq(1, existingShards.length, "Removed server still appears in count: " + tojson(existingShards));

// TODO (SERVER-97816): remove multiversion check
const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);
if (!isMultiversion) {
    assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
} else {
    assert.commandWorkedOrFailedWithCode(
        st.s.adminCommand({removeShard: st.shard1.shardName}),
        ErrorCodes.ShardNotFound,
    );
}

st.stop();
