// Verify that we can run various forms of the mapReduce command during different stages of the
// cluster upgrade process.
//
// @tags: [fix_for_fcv_46]

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

const testName = "map_reduce_multiversion_cluster";
const dbName = "test_" + testName;
const collName = testName;

// Start a sharded cluster in which all mongod and mongos processes are "last-stable" binVersion.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, binVersion: "last-stable"},
    other: {mongosOptions: {binVersion: "last-stable"}}
});

let mongosConn = st.s;
assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Shard the test collection and split it into two chunks.
st.shardColl(collName,
             {state: 1} /* Shard key */,
             {state: "MA"} /* Split at */,
             {state: "MA"} /* Move the chunk containing {state: MA} to its own shard */,
             dbName,
             true /* Wait until documents orphaned by the move get deleted */);

// Seed the source collection with example user documents.
let sourceColl = mongosConn.getDB(dbName)[collName];
const nDocs = 100;
const states = ["AL", "MA", "NY"];

Random.setRandomSeed();
const bulk = sourceColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const randomState = states[Math.floor(Math.random() * states.length)];
    bulk.insert({state: randomState, age: Random.randInt(100)});
}
assert.commandWorked(bulk.execute());

// Runs a set of MapReduce commands which are expected to be supported regardless of the FCV and
// binary version of the nodes in the cluster.
function runValidMrTests(coll) {
    // Map/reduce/finalize functions to compute the average age per state.
    function map() {
        emit(this.state, {count: 1, avgAge: 0, total: this.age});
    }
    function reduce(key, values) {
        let reducedObject = {total: 0, count: 0, avgAge: 0};
        values.forEach(function(value) {
            reducedObject.total += value.total;
            reducedObject.count += value.count;
        });
        return reducedObject;
    }
    function fin(key, reducedValue) {
        if (reducedValue.count > 0) {
            reducedValue.avgAge = reducedValue.total / reducedValue.count;
        }
        return reducedValue;
    }

    function assertResultsValid(results, expectedCount) {
        assert.gt(results.length, 0, tojson(results));
        assert.lte(results.length, expectedCount, tojson(results));
        results.map(resultDoc => assert.eq(resultDoc.value.avgAge,
                                           resultDoc.value.total / resultDoc.value.count,
                                           tojson(results)));
    }

    // Inline output.
    let res = assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: {inline: 1}}));
    assertResultsValid(res.results, states.length);

    // Output mode "replace" to a non-existent unsharded collection.
    const replaceColl = coll.getDB().mr_replace_out;
    assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: replaceColl.getName()}));
    res = replaceColl.find().toArray();
    assertResultsValid(res, states.length);

    // Output mode "replace" to an existing unsharded collection.
    assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: replaceColl.getName()}));
    res = replaceColl.find().toArray();
    assertResultsValid(res, states.length);

    function testMergeAgainstColl(mergeColl) {
        // Output mode "merge" to a non-existent unsharded collection.
        mergeColl.drop();
        assert.commandWorked(coll.mapReduce(
            map,
            reduce,
            {finalize: fin, out: {merge: mergeColl.getName(), db: mergeColl.getDB().getName()}}));
        res = mergeColl.find().toArray();
        assertResultsValid(res, states.length);

        // Cache a sample result document to ensure that re-reducing actually occurs below.
        let sampleDoc = mergeColl.findOne();

        // Output mode "reduce" to an existing unsharded collection.
        assert.commandWorked(coll.mapReduce(
            map,
            reduce,
            {finalize: fin, out: {reduce: mergeColl.getName(), db: mergeColl.getDB().getName()}}));
        res = mergeColl.find().toArray();
        assertResultsValid(res, states.length);
        assert.gte(mergeColl.findOne({_id: sampleDoc._id}).value.avgAge, sampleDoc.value.avgAge);

        // Drop and recreate the target collection as sharded.
        mergeColl.drop();
        st.shardColl(mergeColl.getName(),
                     {_id: 1} /* Shard key */,
                     {_id: "MA"} /* Split at */,
                     {_id: "MA"} /* Move the chunk containing {state: MA} to its own shard */,
                     mergeColl.getDB().getName());

        // Insert sentinel docs in the output collection to ensure that output mode "merge" does
        // not blow it away. To workaround SERVER-44477, ensure that there is at least one document
        // on each shard.
        assert.commandWorked(mergeColl.insert({_id: "PA", value: {total: 5, avgAge: 5, count: 1}}));
        assert.commandWorked(
            mergeColl.insert({_id: "AL", value: {total: 50, avgAge: 50, count: 1}}));

        // Output mode "merge" to an existing sharded collection.
        assert.commandWorked(coll.mapReduce(map, reduce, {
            finalize: fin,
            out: {merge: mergeColl.getName(), db: mergeColl.getDB().getName(), sharded: true}
        }));
        res = mergeColl.find().toArray();
        assertResultsValid(res, states.length + 2);
        assert.eq(mergeColl.find({_id: "PA"}).itcount(), 1);

        // Cache a sample result document to ensure that re-reducing actually occurs below.
        sampleDoc = mergeColl.findOne({_id: {$not: {$in: ["AL", "PA"]}}});

        // Output mode "reduce" to an existing sharded collection.
        assert.commandWorked(coll.mapReduce(map, reduce, {
            finalize: fin,
            out: {reduce: mergeColl.getName(), db: mergeColl.getDB().getName(), sharded: true}
        }));
        res = mergeColl.find().toArray();
        assertResultsValid(res, states.length + 2);
        assert.gte(mergeColl.findOne({_id: sampleDoc._id}).value.avgAge, sampleDoc.value.avgAge);
    }

    // Test merge to a collection in the same database as the source collection.
    testMergeAgainstColl(coll.getDB().mr_merge_out);

    // Test merge to a collection in a foreign database. Creating the collection will also
    // implicitly create the database.
    assert.commandWorked(
        coll.getDB().getSiblingDB("foreign_db").runCommand({create: "mr_merge_out"}));
    testMergeAgainstColl(coll.getDB().getSiblingDB("foreign_db").mr_merge_out);
}

//
// Test against an all 'last-stable' cluster.
//
runValidMrTests(st.s.getDB(dbName)[collName]);

//
// Upgrade the config servers and the shards to the "latest" binVersion.
//
st.upgradeCluster(
    "latest",
    {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false, waitUntilStable: true});

//
// Test against a mixed version cluster where the shards are upgraded to the latest binary but still
// in FCV 'last-stable'. Mongos is still on the 'last-stable' binary version.
//
runValidMrTests(st.s.getDB(dbName)[collName]);

//
// Upgrade mongos to the "latest" binVersion but keep the old FCV.
//
st.upgradeCluster(
    "latest",
    {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true, waitUntilStable: true});

//
// Test against a cluster where both mongos and the shards are upgraded to the latest binary
// version, but remain in the old FCV.
//
runValidMrTests(st.s.getDB(dbName)[collName]);

//
// Fully upgraded to 'latest'.
//
assert.commandWorked(st.s.getDB(dbName).adminCommand({setFeatureCompatibilityVersion: latestFCV}));
runValidMrTests(st.s.getDB(dbName)[collName]);

st.stop();
}());
