/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable on a sharded cluster.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({mongos: 1, shards: 2});

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
st.adminCommand({enableSharding: "test", primaryShard: st.rs0.getURL()});

var db = st.s.getDB("test");

const numdocs = 1000;

const coll = db[jsTest.name()];
const otherColl = db[coll.getName() + "_other"];

for (let testColl of [coll, otherColl]) {
    testColl.createIndex({_id: 1}, {unique: true});

    st.adminCommand({shardcollection: testColl.getFullName(), key: {_id: 1}});
    st.adminCommand({split: testColl.getFullName(), middle: {_id: numdocs / 2}});

    st.adminCommand({
        moveChunk: testColl.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
        _waitForDelete: true,
    });
    st.adminCommand({
        moveChunk: testColl.getFullName(),
        find: {_id: numdocs / 2},
        to: st.shard0.shardName,
        _waitForDelete: true,
    });
}

// Insert simple documents into the main test collection. Aggregation and view pipelines will
// augment these docs with time-based fields.
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numdocs; ++i) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Insert into another collection with pre-made fields for testing the find() command.
bulk = otherColl.initializeUnorderedBulkOp();
const timeFieldValue = new Date();
for (let i = 0; i < numdocs; ++i) {
    bulk.insert({_id: i, timeField: timeFieldValue, clusterTimeField: new Timestamp(0, 1)});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(db.createView("viewWithNow", coll.getName(), [{$addFields: {timeField: "$$NOW"}}]));
const viewWithNow = db["viewWithNow"];

assert.commandWorked(
    db.createView("viewWithClusterTime", coll.getName(), [{$addFields: {timeField: "$$CLUSTER_TIME"}}]),
);
const viewWithClusterTime = db["viewWithClusterTime"];

function toResultsArray(queryRes) {
    return Array.isArray(queryRes) ? queryRes : queryRes.toArray();
}

function runTests({query, expectedNumDocs = numdocs}) {
    const results = toResultsArray(query());
    assert.eq(results.length, expectedNumDocs);

    // Make sure the values are the same for all documents
    for (let i = 0; i < expectedNumDocs; ++i) {
        assert.eq(results[0].timeField, results[i].timeField);
    }

    // Sleep for a while and then rerun.
    sleep(3000);

    const resultsLater = toResultsArray(query());
    assert.eq(resultsLater.length, expectedNumDocs);

    // Later results should be later in time.
    assert.lte(results[0].timeField, resultsLater[0].timeField);
}

function baseCollectionNowFind() {
    return otherColl.find({$expr: {$lte: ["$timeField", "$$NOW"]}});
}

function baseCollectionClusterTimeFind() {
    // The test validator examines 'timeField', so we copy clusterTimeField into timeField here.
    const results = otherColl.find({$expr: {$lt: ["$clusterTimeField", "$$CLUSTER_TIME"]}}).toArray();
    results.forEach((val, idx) => {
        results[idx].timeField = results[idx].clusterTimeField;
    });
    return results;
}

function baseCollectionNowAgg() {
    return coll.aggregate([{$addFields: {timeField: "$$NOW"}}]);
}

function baseCollectionClusterTimeAgg() {
    return coll.aggregate([{$addFields: {timeField: "$$CLUSTER_TIME"}}]);
}

function baseCollectionNowUnion() {
    return coll.aggregate([
        {$addFields: {timeField: "$$NOW"}},
        {$unionWith: {coll: otherColl.getName(), pipeline: [{$addFields: {timeField: "$$NOW"}}]}},
    ]);
}

function baseCollectionClusterTimeUnion() {
    return coll.aggregate([
        {$addFields: {timeField: "$$CLUSTER_TIME"}},
        {
            $unionWith: {coll: otherColl.getName(), pipeline: [{$addFields: {timeField: "$$CLUSTER_TIME"}}]},
        },
    ]);
}

function fromViewWithNow() {
    return viewWithNow.find();
}

function fromViewWithClusterTime() {
    return viewWithClusterTime.find();
}

function withExprNow() {
    return viewWithNow.find({$expr: {$eq: ["$timeField", "$$NOW"]}});
}

function withExprClusterTime() {
    return viewWithClusterTime.find({$expr: {$eq: ["$timeField", "$$CLUSTER_TIME"]}});
}

// $$NOW
runTests({query: baseCollectionNowFind});
runTests({query: baseCollectionNowAgg});
runTests({query: baseCollectionNowUnion, expectedNumDocs: 2 * numdocs});
runTests({query: fromViewWithNow});
runTests({query: withExprNow});

// Test that $$NOW can be used in explain for both find and aggregate.
assert.commandWorked(
    coll
        .explain()
        .find({$expr: {$lte: ["$timeField", "$$NOW"]}})
        .finish(),
);
assert.commandWorked(
    viewWithNow
        .explain()
        .find({$expr: {$eq: ["$timeField", "$$NOW"]}})
        .finish(),
);
assert.commandWorked(coll.explain().aggregate([{$addFields: {timeField: "$$NOW"}}]));

// $$CLUSTER_TIME
runTests({query: baseCollectionClusterTimeFind});
runTests({query: baseCollectionClusterTimeAgg});
runTests({query: baseCollectionClusterTimeUnion, expectedNumDocs: 2 * numdocs});
runTests({query: fromViewWithClusterTime});
runTests({query: withExprClusterTime});

// Test that $$CLUSTER_TIME can be used in explain for both find and aggregate.
assert.commandWorked(
    coll
        .explain()
        .find({$expr: {$lte: ["$timeField", "$$CLUSTER_TIME"]}})
        .finish(),
);
assert.commandWorked(
    viewWithNow
        .explain()
        .find({$expr: {$eq: ["$timeField", "$$CLUSTER_TIME"]}})
        .finish(),
);
assert.commandWorked(coll.explain().aggregate([{$addFields: {timeField: "$$CLUSTER_TIME"}}]));

st.stop();
