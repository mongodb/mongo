/*
 * Tests that a change stream on collection X doesn't erroneously see resharding events which occur
 * on collection Y. Exercises the fix for SERVER-64780.
 * @tags: [
 *     uses_change_streams,
 *     requires_fcv_50
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/chunk_manipulation_util.js");   // For pauseMigrateAtStep, waitForMigrateStep and
                                                   // unpauseMigrateAtStep.

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const dbName = jsTestName();
const reshardCollName = "coll_reshard";
const otherCollName = "coll_other";

const mongosDB = st.s.getDB(dbName);
const mongosReshardColl = mongosDB[reshardCollName];

const mongosOtherColl = mongosDB[otherCollName];
const shardOtherColl = st.rs0.getPrimary().getDB(dbName)[otherCollName];

// Open a {showMigrationEvents:true} change stream directly on the shard, monitoring events on
// 'coll_other'.
const shardOtherCollCsCursor =
    shardOtherColl.aggregate([{$changeStream: {showMigrationEvents: true}}]);

// Drop, recreate, and shard the 'coll_reshard' collection.
assertDropAndRecreateCollection(mongosDB, reshardCollName);

st.ensurePrimaryShard(dbName, st.rs0.name);
st.shardColl(mongosReshardColl, {a: 1}, {a: 50});

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(mongosReshardColl.insert({a: i, b: -i}));
}

// Reshard the 'coll_reshard' collection on {b: 1}.
assert.commandWorked(
    mongosDB.adminCommand({reshardCollection: mongosReshardColl.getFullName(), key: {b: 1}}));

// Confirm that the change stream we opened on 'coll_other' only sees the sentinel 'insert' but does
// not see the earlier 'reshardBegin' or 'reshardDoneCatchUp' events on the 'coll_reshard'
// collection.
assert.commandWorked(mongosOtherColl.insert({_id: "sentinel_write"}));

assert.soon(() => shardOtherCollCsCursor.hasNext());
assert.eq(shardOtherCollCsCursor.next().operationType, "insert");

st.stop();
})();
