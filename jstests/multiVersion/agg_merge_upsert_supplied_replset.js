/**
 * Tests that $merge with {whenMatched: [], whenNotMatched: 'insert'} is handled correctly during
 * upgrade from and downgrade to a pre-backport version of 4.2 on a single replica set.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.
load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

const preBackport42Version = "4.2.1";
const latestVersion = "latest";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {binVersion: preBackport42Version},
});
if (!startSetIfSupportsReadMajority(rst)) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    rst.stopSet();
    return;
}
rst.initiate();

// Obtain references to the test database and create the test collection.
let testDB = rst.getPrimary().getDB(jsTestName());
let sourceColl = testDB.source_coll;
let targetColl = testDB.target_coll;

// Up- or downgrades the replset and then refreshes our references to the test collection.
function refreshReplSet(version, secondariesOnly) {
    // Upgrade the set and wait for it to become available again.
    if (secondariesOnly) {
        rst.upgradeSecondaries({binVersion: version});
    } else {
        rst.upgradeSet({binVersion: version});
    }
    rst.awaitSecondaryNodes();

    // Having upgraded the set, reacquire references to the db and collection.
    testDB = rst.getPrimary().getDB(jsTestName());
    sourceColl = testDB.source_coll;
    targetColl = testDB.target_coll;
}

// Insert a set of test data.
for (let i = -20; i < 20; ++i) {
    assert.commandWorked(sourceColl.insert({_id: i}));
}

// The 'whenMatched' pipeline to apply as part of the $merge. When the old 4.2.1 behaviour is in
// effect, output documents will all have an _id field and the field added by this pipeline.
const mergePipe = [{$addFields: {docWasGeneratedFromWhenMatchedPipeline: true}}];

// Generate the array of output documents we expect to see under the old upsert behaviour.
const expectedOldBehaviourOutput = Array.from(sourceColl.find().toArray(), (doc) => {
    return {_id: doc._id, docWasGeneratedFromWhenMatchedPipeline: true};
});

// The pipeline to run for each test. Results in different output depending on upsert mode used.
const finalPipeline =
    [{$merge: {into: targetColl.getName(), whenMatched: mergePipe, whenNotMatched: "insert"}}];

// Run a $merge with the whole cluster on 'preBackport42Version' and confirm that the output
// documents are produced using the old upsert behaviour.
sourceColl.aggregate(finalPipeline);
assert.sameMembers(targetColl.find().toArray(), expectedOldBehaviourOutput);
assert.commandWorked(targetColl.remove({}));

// Upgrade the Secondaries but leave the Primary on 'preBackport42Version'. The set continues to
// produce output documents using the old upsert behaviour.
refreshReplSet(latestVersion, true);
sourceColl.aggregate(finalPipeline);
assert.sameMembers(targetColl.find().toArray(), expectedOldBehaviourOutput);
assert.commandWorked(targetColl.remove({}));

// Even though we can run $merge on a secondary, this will fail an FCV check because the primary
// is not upgraded to 4.4.
const error = assert.throws(
    () => rst.getSecondaries()[0].getCollection(sourceColl.getFullName()).aggregate(finalPipeline));
assert.commandFailedWithCode(error, 31476);

// Upgrade the Primary to latest. We should now see that the $merge adopts the new behaviour, and
// inserts the exact source document rather than generating one from the whenMatched pipeline.
refreshReplSet(latestVersion);
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV}));
sourceColl.aggregate(finalPipeline);
assert.sameMembers(targetColl.find().toArray(), sourceColl.find().toArray());
assert.commandWorked(targetColl.remove({}));

rst.stopSet();
})();
