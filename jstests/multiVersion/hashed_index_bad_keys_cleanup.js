/**
 * Prior to SERVER-44050, for hashed indexes, if there is an array along index field path, we did
 * not fail insertion. We incorrectly stored empty index key value for those cases. This lead to
 * corruption of index keys.
 *
 * In this test we verify that we are able to successfully update and delete documents that were
 * involved in creating corrupt indexes.
 *
 * When we branch for 4.4, this test should be deleted in the master branch. In the v4.4 branch it
 * should test a 4.2 => 4.4 upgrade scenario.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");         // For upgradeSet.
load("jstests/multiVersion/libs/verify_versions.js");  // For binVersion.
load("jstests/noPassthrough/libs/index_build.js");     // For waitForIndexBuildToStop.

const preBackportVersion = "4.2.1";
const preBackportNodeOptions = {
    binVersion: preBackportVersion
};
const nodeOptionsOfLatestVersion = {
    binVersion: "latest"
};

// Set up a new replSet consisting of 2 nodes, initially running on bad binaries.
const rst = new ReplSetTest({nodes: 2, nodeOptions: preBackportNodeOptions});
rst.startSet();
rst.initiate();

let testDB = rst.getPrimary().getDB(jsTestName());
const collName = "coll";
let coll = testDB[collName];
coll.drop();

// Verify that the replset is on binary version specified in 'preBackportVersion'.
assert.binVersion(testDB.getMongo(), preBackportVersion);

// Insert bad documents using older version.
const indexName = "pqr_hashed";
assert.commandWorked(coll.createIndex({"p.q.r": "hashed"}, {"name": indexName}));
assert.commandWorked(coll.insert({_id: 1, p: []}));
assert.commandWorked(coll.insert({_id: 2, p: {q: [1]}}));
assert.commandWorked(coll.insert({_id: 3, p: [{q: 1}]}));
assert.commandWorked(coll.insert({_id: 4, a: 1, p: [{q: 1}]}));
assert.commandWorked(coll.insert({_id: 5, a: 1, p: [{q: 1}]}));

// Assert that the collection has expected number of documents and index keys.
function assertCollectionHasExpectedDocs(targetColl, expectedNumDocs) {
    const collState = {
        documents: targetColl.find().toArray(),
        indexKeys: targetColl.find().hint({"p.q.r": "hashed"}).returnKey().toArray()
    };
    assert.eq(collState.documents.length, expectedNumDocs, collState);
    assert.eq(collState.indexKeys.length, expectedNumDocs, collState);
}

// Assert that both nodes in the replica set have the expected number of documents/index keys.
function assertBothNodesHaveExpectedDocs(expectedNumDocs) {
    // Wait for the index build to stop on the primary.
    IndexBuildTest.waitForIndexBuildToStop(testDB, collName, indexName);
    assertCollectionHasExpectedDocs(coll, expectedNumDocs);

    rst.awaitReplication();
    const secondaryDB = rst.getSecondary().getDB(jsTestName());
    const secondaryColl = secondaryDB[collName];
    // Wait for the index build to stop on the secondary.
    IndexBuildTest.waitForIndexBuildToStop(secondaryDB, collName, indexName);
    assertCollectionHasExpectedDocs(secondaryColl, expectedNumDocs);
}

// Verify that the documents inserted have the corresponding index keys.
assertBothNodesHaveExpectedDocs(5);

// Helper function which runs validate() on primary and secondary nodes, then verifies that the
// command returned the expected result.
function assertValidateCmdReturned(expectedResult) {
    const resFromPrimary = assert.commandWorked(coll.validate({full: true}));
    assert.eq(resFromPrimary.valid, expectedResult, resFromPrimary);

    rst.awaitReplication();
    const testDBOnSecondary = rst.getSecondary().getDB(jsTestName());
    const resFromSecondary = assert.commandWorked(testDBOnSecondary.coll.validate({full: true}));
    assert.eq(resFromSecondary.valid, expectedResult, resFromSecondary);
}

// Confirm that validate() does not perceive a problem with the malformed documents.
assertValidateCmdReturned(true);

// Upgrade the set to the new binary version.
rst.upgradeSet(nodeOptionsOfLatestVersion);
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[collName];

// Verify that the five documents inserted earlier have their index keys after upgrade.
assertCollectionHasExpectedDocs(coll, 5);

// Verify that after upgrade, inserting bad documents is not allowed.
const arrayAlongPathFailCode = 16766;
assert.commandFailedWithCode(coll.insert({p: []}), arrayAlongPathFailCode);
assert.commandFailedWithCode(coll.insert({p: [{q: 1}]}), arrayAlongPathFailCode);
assert.commandFailedWithCode(coll.insert({p: {q: {r: [3]}}}), arrayAlongPathFailCode);

// After upgrade, validate() should now fail since there are existing bad documents.
assertValidateCmdReturned(false);

// Deleting bad documents succeeds.
assert.commandWorked(coll.deleteOne({_id: 1}));
assert.commandWorked(coll.deleteMany({a: 1}));

// Updating documents to contain array along field path should fail.
assert.commandFailedWithCode(coll.update({_id: 2}, {p: {q: [{r: 1}]}}), arrayAlongPathFailCode);
assert.commandFailedWithCode(
    testDB.runCommand({findAndModify: coll.getName(), query: {_id: 2}, update: {p: {q: [{r: 1}]}}}),
    arrayAlongPathFailCode);

assert.commandFailedWithCode(coll.update({_id: 2}, {p: {q: {r: [3]}}}), arrayAlongPathFailCode);
assert.commandFailedWithCode(testDB.runCommand({
    update: coll.getName(),
    updates: [
        {q: {_id: 3}, u: {$set: {p: {q: [{r: 1}]}}}},
        {q: {_id: 2}, u: {$set: {p: {q: [{r: 1}]}}}}
    ],
    ordered: false
}),
                             arrayAlongPathFailCode);

// Verify that updating to a valid index field works.
assert.commandWorked(coll.update({_id: 2}, {p: {q: {r: 4}}}));

// Verify that the index key is updated correctly by quering with hashed index.
let res = coll.find({"p.q.r": 4}).hint({"p.q.r": "hashed"}).toArray();
assert.eq(res, [{_id: 2, p: {q: {r: 4}}}]);

// Validate should still fail since a bad document {_id: 3} exists.
assertValidateCmdReturned(false);

// Delete the last remaining bad document.
assert.commandWorked(coll.deleteOne({_id: 3}));

// Now that all the bad documents are deleted or updated, verify that validate succeeds.
assertValidateCmdReturned(true);

// Verify that there is only one index key left (for {_id: 2}).
res = coll.find().hint({"p.q.r": "hashed"}).returnKey().itcount();
assert.eq(res, 1);

rst.awaitReplication();
rst.stopSet();
}());