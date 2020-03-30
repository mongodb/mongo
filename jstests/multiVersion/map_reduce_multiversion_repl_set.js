// Verify that we can run various forms of the mapReduce command during different stages of the
// replica set upgrade process.
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // Used by upgradeSet.

const testName = "map_reduce_multiversion_repl_set";
const dbName = "test_" + testName;
const collName = testName;

TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

// Set up a new replSet consisting of 3 nodes, initially running on 'last-stable' binaries.
const rst = new ReplSetTest({nodes: 3, nodeOptions: {binVersion: "last-stable"}});
rst.startSet();
rst.initiate();

// Seed the source collection with example user documents.
let sourceDB = rst.getPrimary().getDB(dbName);
let sourceColl = sourceDB[collName];
sourceColl.drop();

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
// binary version of the nodes.
function runValidMrTests(db, coll) {
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

    function assertResultsValid(results) {
        assert.gt(results.length, 0);
        assert.lte(results.length, states.length);
        results.map(resultDoc => assert.eq(resultDoc.value.avgAge,
                                           resultDoc.value.total / resultDoc.value.count));
    }

    // Inline output.
    let res = assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: {inline: 1}}));
    assertResultsValid(res.results);

    // Output mode "replace" to a non-existent collection.
    const replaceColl = "mr_replace_out";
    assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: replaceColl}));
    res = coll.getDB()[replaceColl].find().toArray();
    assertResultsValid(res);

    // Output mode "replace" to an existing collection.
    assert.commandWorked(coll.mapReduce(map, reduce, {finalize: fin, out: replaceColl}));
    res = coll.getDB()[replaceColl].find().toArray();
    assertResultsValid(res);

    // Output mode "merge" to a non-existent collection.
    const mergeColl = db.mr_merge_out;
    mergeColl.drop();
    assert.commandWorked(
        coll.mapReduce(map, reduce, {finalize: fin, out: {merge: mergeColl.getName()}}));
    res = mergeColl.find().toArray();
    assertResultsValid(res);

    // Cache a sample result document to ensure that re-reducing actually occurs below.
    const sampleDoc = mergeColl.findOne();

    // Output mode "reduce" to an existing collection.
    assert.commandWorked(
        coll.mapReduce(map, reduce, {finalize: fin, out: {reduce: mergeColl.getName()}}));
    assert.gte(mergeColl.findOne({_id: sampleDoc._id}).value.avgAge, sampleDoc.value.avgAge);
}

//
// Binary version 'last-stable' and FCV 'last-stable'.
//
runValidMrTests(sourceDB, sourceColl);

// Upgrade the set to the new binary version, but keep the feature compatibility version at
// 'last-stable'.
rst.upgradeSet({binVersion: "latest"});
sourceDB = rst.getPrimary().getDB(dbName);
sourceColl = sourceDB[collName];

//
// Binary version 'latest' and FCV 'last-stable'.
//
runValidMrTests(sourceDB, sourceColl);

//
// Binary version 'latest' and FCV 'latest'.
//
assert.commandWorked(sourceDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
runValidMrTests(sourceDB, sourceColl);

rst.awaitReplication();
rst.stopSet();
}());
