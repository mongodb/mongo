/**
 * Test to verify the covering behaviour of compound hashed index on a cluster sharded with compound
 * hashed shard key.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertStagesForExplainOfCommand().

const st = new ShardingTest({shards: 2});
const kDbName = jsTestName();
const coll = st.s.getDB(kDbName)["coll"];

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

/**
 * Runs find command with the 'filter', 'projection' and 'hint' parameters. Then validates that the
 * output returned matches 'expectedOutput'. Also runs explain() command on the same find command
 * and validates that all the 'expectedStages' are present and all the 'stagesNotExpected' are not
 * present in the plan returned.
 */
function validateFindCmdOutputAndPlan({
    filter,
    projection = {
        _id: 0
    },
    hint,
    expectedStages,
    stagesNotExpected,
    expectedOutput
}) {
    const cmdObj = Object.assign({find: coll.getName(), filter: filter, projection: projection},
                                 hint ? {hint: hint} : null);
    const res = assert.commandWorked(coll.runCommand(cmdObj));
    const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

    // We ignore the order since hashed index order is not predictable.
    assert.sameMembers(expectedOutput, ouputArray);
    assertStagesForExplainOfCommand({coll, cmdObj, expectedStages, stagesNotExpected});
}

//
// Test to validate that the orphan documents are correctly rejected during shard filter analysis.
//
assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1, b: "hashed", c: 1}}));
assert.commandWorked(
    st.s.adminCommand({split: coll.getFullName(), middle: {a: 0, b: NumberLong(0), c: MinKey}}));

// Postive hashed values of 'b' should go to 'shard1DB' and negative value should go to 'shard0DB'
assert.commandWorked(st.s.adminCommand({
    moveChunk: coll.getFullName(),
    bounds: [{a: 0, b: NumberLong(0), c: MinKey}, {a: MaxKey, b: MaxKey, c: MaxKey}],
    to: st.shard1.shardName
}));

// Make sure that we have at least one valid document and one orphan document. The hashed value of 0
// is postive and hence this document should belong to shard1.
let validDocs = [{a: 0, b: 0, c: "valid"}];
assert.gt(convertShardKeyToHashed(validDocs[0]['b']), 0);

// The hashed value of 3 is negative and hence should be an orphan on shard1.
let orphanDocs = [{a: 0, b: 3, c: "orphan"}];
assert.lt(convertShardKeyToHashed(orphanDocs[0]['b']), 0);

// Generate 100 more documents and distribute them between 'validDocs' and 'orphanDocs' based on the
// hashed value of 'b'.
for (let i = 0; i < 100; i++) {
    // Generate a random number between 0 and 1000000.
    const insertObj = {a: 0, b: Math.floor(Math.random() * 1000000), c: 'value'};
    if (convertShardKeyToHashed(insertObj['b']) < 0) {
        orphanDocs.push(insertObj);
    } else {
        validDocs.push(insertObj);
    }
}

// Insert documents by directly connecting to the shard1 so that we can explicity create orphan
// documents. We then run a 'find' command by connecting to mongos and validate that the orphan
// documents are correctly rejected.
const shard1DB = st.rs1.getPrimary().getDB(kDbName);
assert.commandWorked(shard1DB.coll.insertMany(validDocs, {ordered: false}));
assert.commandWorked(shard1DB.coll.insertMany(orphanDocs, {ordered: false}));
// We do not project 'b' so that the query can be covered.
for (let validDoc of validDocs) {
    delete validDoc.b;
}
validateFindCmdOutputAndPlan({
    filter: {a: 0},
    projection: {a: 1, c: 1, _id: 0},
    expectedOutput: validDocs,  // Ophan documents are not returned.
    expectedStages: ["IXSCAN", "SHARD_MERGE", "SHARDING_FILTER"],
    stagesNotExpected: ["FETCH"]
});
coll.drop();

//
// Tests to validate covering behaviour in the presense of various indexes.
//
assert.commandWorked(st.s.getDB('config').adminCommand(
    {shardCollection: coll.getFullName(), key: {a: 1, b: "hashed", c: 1}}));
assert.commandWorked(
    st.s.adminCommand({split: coll.getFullName(), middle: {a: 0, b: MinKey, c: MinKey}}));

// Postive numbers of 'a' should go to 'shard1DB' and negative numbers should go to 'shard0DB'
assert.commandWorked(st.s.adminCommand({
    moveChunk: coll.getFullName(),
    bounds: [{a: 0, b: MinKey, c: MinKey}, {a: MaxKey, b: MaxKey, c: MaxKey}],
    to: st.shard1.shardName
}));

for (let i = 20; i < 40; i++) {
    assert.commandWorked(coll.insert({a: i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)}));
    assert.commandWorked(
        coll.insert({a: -i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)}));
}

// Verify that the query can be covered if neither the query nor the project uses hashed field.
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, c: 1, _id: 0},
    expectedOutput: [{a: 26, c: 6}, {a: 27, c: 7}, {a: 28, c: 8}],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "SHARDING_FILTER"],
    stagesNotExpected: ["FETCH"]
});
validateFindCmdOutputAndPlan({
    filter: {a: {$gte: -20, $lt: 21}},
    projection: {a: 1, c: 1, _id: 0},
    expectedOutput: [{a: -20, c: 0}, {a: 20, c: 0}],
    expectedStages: ["IXSCAN", "SHARD_MERGE", "SHARDING_FILTER"],
    stagesNotExpected: ["FETCH"]
});

// Verify that a query on hashed field is always fetched, even if the projection does not include
// the hashed field.
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}, b: {subObj: "str_0"}},
    projection: {a: 1, c: 1, _id: 0},
    expectedOutput: [{a: 26, c: 6}],
    expectedStages: ["IXSCAN", "FETCH", "SINGLE_SHARD", "SHARDING_FILTER"]
});

// Verify that the query cannot be covered if the project includes hashed field.
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, b: 1, c: 1, _id: 0},
    expectedOutput: [
        {a: 26, b: {subObj: "str_0"}, c: 6},
        {a: 27, b: {subObj: "str_1"}, c: 7},
        {a: 28, b: {subObj: "str_2"}, c: 8}
    ],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "FETCH", "SHARDING_FILTER"]
});

// Create an index which doesn't include one of the shard key fields and verify that the query is
// fetched.
coll.createIndex({a: 1, c: 1});
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, c: 1, _id: 0},
    hint: {a: 1, c: 1},
    expectedOutput: [{a: 26, c: 6}, {a: 27, c: 7}, {a: 28, c: 8}],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "FETCH", "SHARDING_FILTER"]
});

// Verify that the query cannot be covered if index provides hashed value for a field ('c'), but the
// corresponding shard key field is a range field.
coll.createIndex({a: 1, b: 1, c: "hashed"});
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, _id: 0},
    hint: {a: 1, b: 1, c: "hashed"},
    expectedOutput: [{a: 26}, {a: 27}, {a: 28}],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "FETCH", "SHARDING_FILTER"]
});

// Verify that the query can be covered when index provides range value for a field, but the
// corresponding shard key field is hashed.
coll.createIndex({a: 1, b: 1, c: 1});
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, c: 1, _id: 0},
    hint: {a: 1, b: 1, c: 1},
    expectedOutput: [{a: 26, c: 6}, {a: 27, c: 7}, {a: 28, c: 8}],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "SHARDING_FILTER"],
    stagesNotExpected: ["FETCH"]
});

// Verify that the query can be covered if all the shard key fields are provided by the index, even
// if the order is different.
coll.createIndex({a: 1, c: 1, b: "hashed"});
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}},
    projection: {a: 1, c: 1, _id: 0},
    hint: {a: 1, c: 1, b: "hashed"},
    expectedOutput: [{a: 26, c: 6}, {a: 27, c: 7}, {a: 28, c: 8}],
    expectedStages: ["IXSCAN", "SINGLE_SHARD", "SHARDING_FILTER"],
    stagesNotExpected: ["FETCH"]
});

st.stop();
})();
