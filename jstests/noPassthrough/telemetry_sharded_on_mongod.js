// Tests that queries from mongos to mongod record telemetry correctly on mongod.
// Does not test any mongos logic on the telemetry read path.
// @tags: [requires_sharding, requires_fcv_63]
//
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 2147483647},
};

const conn = MongoRunner.runMongod(options);
let db = conn.getDB(jsTestName());
if (!FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    jsTestLog("Skipping test as featureFlagTelemetry is not enabled");
    MongoRunner.stopMongod(conn);
    return;
}

let coll = db[jsTestName()];
coll.drop();

// Insert documents.
let documents = [];
for (let i = 0; i < 10; i++) {
    documents.push({_id: i, val: i * 5});
}

coll.insert(documents);

// Run a query that pretends to be from mongos. For the purposes of this test we don't care about
// the results. If the command worked, it should show up in the telemetry store.

const queryHashForTestBinA = new BinData(5, "Tfo0EBc0wbGscAbGuj7goA==");
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$match: {_id: 5}}, {$project: {val: false}}],
    fromMongos: true,
    needsMerge: true,
    hashedTelemetryKey: {queryHash: queryHashForTestBinA, hostAndPort: "localHost:1"},
    cursor: {},
}));

const queryHashForTestBinB = new BinData(5, "d4Tk/HbrrGSpWrib6hY+IQ==");
// Do the same, but a find command.
assert.commandWorked(db.runCommand({
    find: coll.getName(),
    filter: {_id: 5},
    sort: {_id: 1},
    projection: {val: false},
    hashedTelemetryKey: {queryHash: queryHashForTestBinB, hostAndPort: "localHost:1"},
}));

// Check the telemetry store.
const telemetryResult = assert.commandWorked(
    db.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}, {$sort: {"key": 1}}], cursor: {}}));
// Assert that the telemetry object has the "hashed key" as a key as expected and not the query
// shape for both commands.
let telemetryResultBatch = telemetryResult.cursor.firstBatch;
assert(telemetryResultBatch[0].key.hasOwnProperty("queryHash"), tojson(telemetryResultBatch[0]));
assert.eq(
    telemetryResultBatch[0].key.queryHash, queryHashForTestBinA, tojson(telemetryResultBatch[0]));
assert(telemetryResultBatch[1].key.hasOwnProperty("queryHash"), tojson(telemetryResultBatch[1]));
assert.eq(
    telemetryResultBatch[1].key.queryHash, queryHashForTestBinB, tojson(telemetryResultBatch[1]));

MongoRunner.stopMongod(conn);

// Spin up a sharded cluster to test end to end.
const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    rsOptions: options,
    shardOptions: options,
    mongosOptions: options,
});

const mongos = st.s;
db = mongos.getDB(jsTestName());
assert(FeatureFlagUtil.isEnabled(db, "Telemetry"), "featureFlagTelemetry not enabled");
coll = db[jsTestName()];
coll.insert(documents);

// Helper function to run the test regardless of the sharding state of the collection.
function runTestOnDbAndColl(funcDb, funcColl, funcShardTest) {
    // Run an aggregation command.
    assert.commandWorked(funcDb.runCommand({
        aggregate: funcColl.getName(),
        pipeline: [{$match: {_id: 5}}, {$project: {val: false}}],
        cursor: {},
    }));

    // Run a find command.
    assert.commandWorked(funcDb.runCommand({
        find: funcColl.getName(),
        filter: {_id: 5},
        sort: {_id: 1},
        projection: {val: false},
    }));

    const mongod = funcShardTest.getPrimaryShard(funcDb.getName());
    const shardDB = mongod.getDB(jsTestName());
    // Check that these commands generated something in the shard telemetry store.
    const shardedTelemetryResult = assert.commandWorked(shardDB.adminCommand({
        aggregate: 1,
        pipeline: [
            {$telemetry: {}},
            {$match: {"key.queryHash": {"$exists": true}}},
            {$sort: {"key.queryHash": 1}}
        ],
        cursor: {}
    }));

    let telemetryResultBatch = shardedTelemetryResult.cursor.firstBatch;
    // We can't check the value of hostandport as we may be assigned a different value each run. The
    // hash should be deterministic.
    // Note that this test is for checking mongod, not mongos. We are not checking whether mongos
    // correctly aggregates results, just that mongod has a hashed key.
    assert(telemetryResultBatch[0].key.hasOwnProperty("queryHash"),
           tojson(telemetryResultBatch[0]));
    assert.eq(telemetryResultBatch[0].key.queryHash,
              queryHashForTestBinA,
              tojson(telemetryResultBatch[1]));
    assert(telemetryResultBatch[1].key.hasOwnProperty("queryHash"),
           tojson(telemetryResultBatch[0]));
    assert.eq(telemetryResultBatch[1].key.queryHash,
              queryHashForTestBinB,
              tojson(telemetryResultBatch[1]));
}
runTestOnDbAndColl(db, coll, st);

// Restart the cluster to clear the telemetry store.
st.stop();

// We're going to shard the collection, so have two shards this time.
const shardedST = new ShardingTest({
    mongos: 1,
    shards: 2,
    config: 1,
    rs: {nodes: 1},
    rsOptions: options,
    mongosOptions: options,
});

const shardedMongos = shardedST.s;
db = shardedMongos.getDB(jsTestName());
assert(FeatureFlagUtil.isEnabled(db, "Telemetry"), "featureFlagTelemetry not enabled");
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
coll = db[jsTestName()];
coll.insert(documents);
assert.commandWorked(
    shardedMongos.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
// Ensure docs on each shard.
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
assert.commandWorked(shardedMongos.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 3}, to: shardedST.shard0.shardName}));
assert.commandWorked(shardedMongos.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 7}, to: shardedST.shard1.shardName}));

runTestOnDbAndColl(db, coll, shardedST);
shardedST.stop();
}());
