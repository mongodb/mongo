// Test mongos implementation of time-limited operations: verify that mongos correctly forwards max
// time to shards, and that mongos correctly times out max time sharded getmore operations (which
// are run in parallel on shards).
// @tags: [
//   requires_replication,
// ]
(function() {
'use strict';

var st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1},
    // The maxTimeAlwaysTimeOut failpoint interferes with the maxAwaitTimeMS parameter sent by the
    // streamable RSM so we have mongos use the non-streamable version here.
    mongosOptions: {setParameter: {replicaSetMonitorProtocol: "sdam"}},
});

var mongos = st.s0;
var shards = [st.shard0, st.shard1];
var coll = mongos.getCollection("foo.bar");
var admin = mongos.getDB("admin");
var cursor;
var res;

// Helper function to configure "maxTimeAlwaysTimeOut" fail point on shards, which forces mongod
// to throw if it receives an operation with a max time. See fail point declaration for complete
// description.
var configureMaxTimeAlwaysTimeOut = function(mode) {
    assert.commandWorked(shards[0].getDB("admin").runCommand(
        {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
    assert.commandWorked(shards[1].getDB("admin").runCommand(
        {configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode}));
};

// Helper function to configure "maxTimeAlwaysTimeOut" fail point on shards, which prohibits
// mongod from enforcing time limits. See fail point declaration for complete description.
var configureMaxTimeNeverTimeOut = function(mode) {
    assert.commandWorked(shards[0].getDB("admin").runCommand(
        {configureFailPoint: "maxTimeNeverTimeOut", mode: mode}));
    assert.commandWorked(shards[1].getDB("admin").runCommand(
        {configureFailPoint: "maxTimeNeverTimeOut", mode: mode}));
};

//
// Pre-split collection: shard 0 takes {_id: {$lt: 0}}, shard 1 takes {_id: {$gte: 0}}.
//
assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
st.ensurePrimaryShard(coll.getDB().toString(), st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

//
// Insert 1000 documents into sharded collection, such that each shard owns 500.
//
const nDocsPerShard = 500;
var bulk = coll.initializeUnorderedBulkOp();
for (var i = -nDocsPerShard; i < nDocsPerShard; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());
assert.eq(nDocsPerShard, shards[0].getCollection(coll.getFullName()).count());
assert.eq(nDocsPerShard, shards[1].getCollection(coll.getFullName()).count());

//
// Test that mongos correctly forwards max time to shards for sharded queries.  Uses
// maxTimeAlwaysTimeOut to ensure mongod throws if it receives a max time.
//

// Positive test.
configureMaxTimeAlwaysTimeOut("alwaysOn");
cursor = coll.find();
cursor.maxTimeMS(60 * 1000);
assert.throws(function() {
    cursor.next();
}, [], "expected query to fail in mongod due to maxTimeAlwaysTimeOut fail point");

// Negative test.
configureMaxTimeAlwaysTimeOut("off");
cursor = coll.find();
cursor.maxTimeMS(60 * 1000);
assert.doesNotThrow(function() {
    cursor.next();
}, [], "expected query to not hit time limit in mongod");

//
// Test that mongos correctly times out max time sharded getmore operations.  Uses
// maxTimeNeverTimeOut to ensure mongod doesn't enforce a time limit.
//

configureMaxTimeNeverTimeOut("alwaysOn");

// ~30s operation with a 10s limit. Each shard will process 250 batches, each of which takes 60ms*2
// = 120ms. We do not expect the first operation (one batch) to time out, but iterating the cursor
// should exhaust the 10s limit.
cursor = coll.find({
    $where: function() {
        sleep(60);
        return true;
    }
});
cursor.batchSize(2);
cursor.maxTimeMS(10 * 1000);
assert.doesNotThrow(
    () => cursor.next(), [], "did not expect mongos to time out first batch of query");
assert.throws(() => cursor.itcount(), [], "expected mongos to abort getmore due to time limit");

// Negative test. ~5s operation, with a high (1-day) limit.
cursor = coll.find({
    $where: function() {
        sleep(10);
        return true;
    }
});
cursor.batchSize(2);
cursor.maxTimeMS(1000 * 60 * 60 * 24);
assert.doesNotThrow(function() {
    cursor.next();
}, [], "did not expect mongos to time out first batch of query");
assert.doesNotThrow(function() {
    cursor.itcount();
}, [], "did not expect getmore ops to hit the time limit");

configureMaxTimeNeverTimeOut("off");

//
// Test that mongos correctly forwards max time to shards for sharded commands.  Uses
// maxTimeAlwaysTimeOut to ensure mongod throws if it receives a max time.
//

// Positive test for "validate".
configureMaxTimeAlwaysTimeOut("alwaysOn");
assert.commandFailedWithCode(
    coll.runCommand("validate", {maxTimeMS: 60 * 1000}),
    ErrorCodes.MaxTimeMSExpired,
    "expected vailidate to fail with code " + ErrorCodes.MaxTimeMSExpired +
        " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

// Negative test for "validate".
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(coll.runCommand("validate", {maxTimeMS: 60 * 1000}),
                     "expected validate to not hit time limit in mongod");

// Positive test for "count".
configureMaxTimeAlwaysTimeOut("alwaysOn");
assert.commandFailedWithCode(
    coll.runCommand("count", {maxTimeMS: 60 * 1000}),
    ErrorCodes.MaxTimeMSExpired,
    "expected count to fail with code " + ErrorCodes.MaxTimeMSExpired +
        " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

// Negative test for "count".
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(coll.runCommand("count", {maxTimeMS: 60 * 1000}),
                     "expected count to not hit time limit in mongod");

// Positive test for "collStats".
configureMaxTimeAlwaysTimeOut("alwaysOn");
assert.commandFailedWithCode(
    coll.runCommand("collStats", {maxTimeMS: 60 * 1000}),
    ErrorCodes.MaxTimeMSExpired,
    "expected collStats to fail with code " + ErrorCodes.MaxTimeMSExpired +
        " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

// Negative test for "collStats".
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(coll.runCommand("collStats", {maxTimeMS: 60 * 1000}),
                     "expected collStats to not hit time limit in mongod");

// Positive test for "mapReduce".
configureMaxTimeAlwaysTimeOut("alwaysOn");
res = coll.runCommand("mapReduce", {
    map: function() {
        emit(0, 0);
    },
    reduce: function(key, values) {
        return 0;
    },
    out: {inline: 1},
    maxTimeMS: 60 * 1000
});
assert.commandFailedWithCode(
    res,
    ErrorCodes.MaxTimeMSExpired,
    "expected mapReduce to fail with code " + ErrorCodes.MaxTimeMSExpired +
        " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

// Negative test for "mapReduce".
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(coll.runCommand("mapReduce", {
    map: function() {
        emit(0, 0);
    },
    reduce: function(key, values) {
        return 0;
    },
    out: {inline: 1},
    maxTimeMS: 60 * 1000
}),
                     "expected mapReduce to not hit time limit in mongod");

// Positive test for "aggregate".
configureMaxTimeAlwaysTimeOut("alwaysOn");
assert.commandFailedWithCode(
    coll.runCommand("aggregate", {pipeline: [], cursor: {}, maxTimeMS: 60 * 1000}),
    ErrorCodes.MaxTimeMSExpired,
    "expected aggregate to fail with code " + ErrorCodes.MaxTimeMSExpired +
        " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

// Negative test for "aggregate".
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(coll.runCommand("aggregate", {pipeline: [], cursor: {}, maxTimeMS: 60 * 1000}),
                     "expected aggregate to not hit time limit in mongod");

// Positive test for "setFeatureCompatibilityVersion"
configureMaxTimeAlwaysTimeOut("alwaysOn");
assert.commandFailedWithCode(
    admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, maxTimeMS: 1000 * 60 * 60 * 24}),
    ErrorCodes.MaxTimeMSExpired,
    "expected setFeatureCompatibilityVersion to fail due to maxTimeAlwaysTimeOut fail point");

// Negative test for "setFeatureCompatibilityVersion"
configureMaxTimeAlwaysTimeOut("off");
assert.commandWorked(
    admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, maxTimeMS: 1000 * 60 * 60 * 24}),
    "expected setFeatureCompatibilityVersion to not hit time limit in mongod");

// Negative test for "setFeatureCompatibilityVersion" to upgrade
assert.commandWorked(
    admin.runCommand({setFeatureCompatibilityVersion: latestFCV, maxTimeMS: 1000 * 60 * 60 * 24}),
    "expected setFeatureCompatibilityVersion to not hit time limit in mongod");

// Test that the maxTimeMS is still enforced on the shards even if we do not spend much time in
// mongos blocking.

// Manually run a find here so we can be sure cursor establishment happens with batch size 0.
const kTimeoutMS = 10 * 1000;
res = assert.commandWorked(coll.runCommand({
    find: coll.getName(),
    filter: {
        $where: function() {
            if (this._id < 0) {
                // Slow down the query only on one of the shards. Each shard has 500 documents
                // so we expect the slow shard to take ~30 seconds to return a batch of 500.
                sleep(60);
            }
            return true;
        }
    },
    maxTimeMS: kTimeoutMS,
    batchSize: 0
}));
// Use a batch size of 500 to allow returning results from the fast shard as soon as they're
// ready, as opposed to waiting to return one 16MB batch at a time.
const kBatchSize = nDocsPerShard;
cursor = new DBCommandCursor(coll.getDB(), res, kBatchSize);
// The fast shard should return relatively quickly.
for (let i = 0; i < nDocsPerShard; ++i) {
    let next = assert.doesNotThrow(
        () => cursor.next(), [], "did not expect mongos to time out first batch of query");
    assert.gte(next._id, 0);
}
// Sleep on the client-side so mongos's time budget is not being used.
sleep(kTimeoutMS);
// Even though mongos has not been blocking this whole time, the shard has been busy computing
// the next batch and should have timed out.
assert.throws(() => cursor.next(), [], "expected mongos to abort getMore due to time limit");

// TODO SERVER-30179: Re-introduce tests for moveChunk and maxTimeMS.

st.stop();
})();
