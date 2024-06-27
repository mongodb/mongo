/**
 * Tests that indexes with no unique/sparse specified are identical to unique:false/sparse:false.
 * On a 5.0 server with FCV set to 4.4, no conflicts should be thrown when creating indexes with
 * any combination of no unique, no sparse, unique:false, or sparse:false.
 */
(function() {
"use strict";

const mongod = MongoRunner.runMongod();
assert.neq(null, mongod, "mongod was unable to start up");
const db = mongod.getDB("test");
const admin = db.getSiblingDB("admin");
try {
    checkFCV(admin, "5.0");
} catch (e) {
    jsTestLog("Expecting FCV 5.0: " + tojson(e));
    quit();
}

// Set FCV 4.4 on a 5.0 server
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

const coll = db.test;

// The key pattern and spec against which other indexes will be compared during createIndexes.
const indexSpec = {
    name: "a_1",
};
const keyPattern = {
    a: 1
};

// Helper function to build an index spec based on 'initialIndexSpec'.
function makeSpec(initialIndexSpec, opts) {
    return Object.assign({}, initialIndexSpec, opts);
}

// Verifies that the number of indexes changed in accordance with the 'expectedChange' argument.
function assertNumIndexesAfterComparedToBefore(cmdRes, expectedChange) {
    assert(cmdRes.numIndexesBefore != null);
    const numIndexesBefore = cmdRes.numIndexesBefore;
    const numIndexesAfter = cmdRes.numIndexesAfter;
    assert.eq(numIndexesAfter, numIndexesBefore + expectedChange);
}

function test() {
    // Create an index on {a: 1}
    let cmdRes = assert.commandWorked(coll.createIndex(keyPattern, indexSpec));
    assertNumIndexesAfterComparedToBefore(cmdRes, 1);

    // Verify that creating an index with unique:false does not return an error.
    cmdRes =
        assert.commandWorked(coll.createIndex(keyPattern, makeSpec(indexSpec, {unique: false})));
    assertNumIndexesAfterComparedToBefore(cmdRes, 0);

    // Verify that creating an index with sparse:false does not return an error.
    cmdRes =
        assert.commandWorked(coll.createIndex(keyPattern, makeSpec(indexSpec, {sparse: false})));
    assertNumIndexesAfterComparedToBefore(cmdRes, 0);

    // Verify that creating an index with unique:false, sparse:false does not return an error.
    cmdRes = assert.commandWorked(
        coll.createIndex(keyPattern, makeSpec(indexSpec, {unique: false, sparse: false})));
    assertNumIndexesAfterComparedToBefore(cmdRes, 0);
}

// Test with FCV 4.4
test();
assert.commandWorked(coll.dropIndex(keyPattern));

// Test with FCV 5.0
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
test();

MongoRunner.stopMongod(mongod);
})();
