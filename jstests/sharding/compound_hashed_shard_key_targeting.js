/**
 * Test to verify that all the CRUD operations get routed to the correct shard when the shard key is
 * compound hashed.
 *
 * @tags: [requires_fcv_44, multiversion_incompatible, requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For assertStagesForExplainOfCommand().
load("jstests/libs/profiler.js");             // For profilerHas*OrThrow helper functions.

const st = new ShardingTest({shards: 2});
const kDbName = jsTestName();
const ns = kDbName + '.coll';

// Enable 'retryWrites' so that the shard key fields are updatable.
const session = st.s.startSession({retryWrites: true});
const sessionDB = session.getDatabase(kDbName);
const coll = sessionDB["coll"];
const shard0DB = st.shard0.getDB(kDbName);
const shard1DB = st.shard1.getDB(kDbName);

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

/**
 * Enables profiling on both shards so that we can verify the targeting behaviour.
 */
function restartProfiling() {
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }
}

/**
 * Runs find command with the 'filter' and validates that the output returned matches
 * 'expectedOutput'. Also runs explain() command on the same find command and validates that all
 * the 'expectedStages' are present in the plan returned.
 */
function validateFindCmdOutputAndPlan({filter, expectedStages, expectedOutput, testName}) {
    restartProfiling();
    const cmdObj = {find: coll.getName(), filter: filter, projection: {_id: 0}, comment: testName};
    const res = assert.commandWorked(coll.runCommand(cmdObj));
    const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

    // We ignore the order since hashed index order is not predictable.
    assert(arrayEq(expectedOutput, ouputArray), ouputArray);
    assertStagesForExplainOfCommand({coll, cmdObj, expectedStages});
}

/**
 * Tests when range field is a prefix of compound hashed shard key.
 */
assert.commandWorked(
    st.s.getDB('config').adminCommand({shardCollection: ns, key: {a: 1, b: "hashed", c: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {a: 0, b: MinKey, c: MinKey}}));

// Postive numbers of 'a' should go to 'shard1DB' and negative numbers should go to 'shard0DB'
assert.commandWorked(st.s.adminCommand({
    moveChunk: ns,
    bounds: [{a: 0, b: MinKey, c: MinKey}, {a: MaxKey, b: MaxKey, c: MaxKey}],
    to: st.shard1.shardName
}));

restartProfiling();

// Test to verify that insert operations are routed to correct shard and succeeds on the respective
// shards.
for (let i = 20; i < 40; i++) {
    assert.commandWorked(coll.insert({a: i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)}));
    profilerHasZeroMatchingEntriesOrThrow({
        profileDB: shard0DB,
        filter: {ns: ns, "command.insert": "coll", "command.documents.a": i}
    });
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard1DB,
        filter: {ns: ns, "command.insert": "coll", "command.documents.a": i, "ninserted": 1}
    });

    assert.commandWorked(
        coll.insert({a: -i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)}));
    profilerHasZeroMatchingEntriesOrThrow({
        profileDB: shard1DB,
        filter: {ns: ns, "command.insert": "coll", "command.documents.a": -i}
    });
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard0DB,
        filter: {ns: ns, "command.insert": "coll", "command.documents.a": -i, "ninserted": 1}
    });
}

// Verify that $in query with all predicates values present in a single shard, can be targeted
// correctly. Also verify that the command uses index scan on the individual shard.
let testName = "FindWithDollarIn";
validateFindCmdOutputAndPlan({
    filter: {
        a: {$in: [-38, -37]},
        b: {$in: [{subObj: "str_12"}, {subObj: "str_11"}]},
        c: {$in: [7, 8]}
    },
    expectedOutput: [{a: -37, b: {subObj: "str_11"}, c: 7}, {a: -38, b: {subObj: "str_12"}, c: 8}],
    expectedStages: ["IXSCAN", "FETCH", "SINGLE_SHARD"],
    testName: testName
});
profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: shard1DB, filter: {ns: ns, "command.find": "coll", "command.comment": testName}});
profilerHasSingleMatchingEntryOrThrow(
    {profileDB: shard0DB, filter: {ns: ns, "command.find": "coll", "command.comment": testName}});

// Verify that a range query on a non-hashed prefix field can target a single shard if all values in
// the range are on that shard. Also verify that the command uses index scan on the individual
// shard.
testName = "Range_query";
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}, b: {subObj: "str_0"}},
    expectedOutput: [{a: 26, b: {subObj: "str_0"}, c: 6}],
    expectedStages: ["IXSCAN", "FETCH", "SINGLE_SHARD"],
    testName: testName
});
profilerHasSingleMatchingEntryOrThrow(
    {profileDB: shard1DB, filter: {ns: ns, "command.find": "coll", "command.comment": testName}});
profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: shard0DB, filter: {ns: ns, "command.find": "coll", "command.comment": testName}});

// Test to verify that the update operation can use query to route the operation. Also verify that
// updating shard key value succeeds.
let updateObj = {a: 22, b: {subObj: "str_0"}, c: "update", p: 1};
let res = assert.commandWorked(coll.update({a: 26, b: {subObj: "str_0"}, c: 6}, updateObj));
assert.eq(res.nModified, 1, res);
assert.eq(coll.count(updateObj), 1);
profilerHasSingleMatchingEntryOrThrow({profileDB: shard1DB, filter: {ns: ns, "op": "update"}});
profilerHasZeroMatchingEntriesOrThrow({profileDB: shard0DB, filter: {ns: ns, "op": "update"}});

// Test to verify that the update operation can use query to route the operation. Also verify that
// updating shard key value succeeds when the document has to move shard.
testName = "updateShardKeyValueWrongShard";
updateObj = {
    $set: {a: -100, p: testName},
};
restartProfiling();
res = assert.commandWorked(coll.update({a: 22, b: {subObj: "str_0"}, c: "update"}, updateObj));
assert.eq(res.nModified, 1, res);

// Verify that the 'update' command gets targeted to 'shard1DB'.
profilerHasAtLeastOneMatchingEntryOrThrow({profileDB: shard1DB, filter: {ns: ns, "op": "update"}});
profilerHasZeroMatchingEntriesOrThrow({profileDB: shard0DB, filter: {ns: ns, "op": "update"}});

// Verify that the 'count' command gets targeted to 'shard0DB' after the update.
assert.eq(coll.count(updateObj["$set"]), 1);
profilerHasSingleMatchingEntryOrThrow(
    {profileDB: shard0DB, filter: {ns: ns, "command.count": "coll"}});
profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: shard1DB, filter: {ns: ns, "command.count": "coll"}});

// Test to verify that the 'delete' command with a range query predicate can target a single shard
// if all values in the range are on that shard.
restartProfiling();
res = assert.commandWorked(coll.remove({a: {$lte: -1}}));
assert.eq(res.nRemoved, 21, res);
assert.eq(coll.count({a: {$lte: -1}}), 0);
profilerHasSingleMatchingEntryOrThrow({profileDB: shard0DB, filter: {ns: ns, "op": "remove"}});
profilerHasZeroMatchingEntriesOrThrow({profileDB: shard1DB, filter: {ns: ns, "op": "remove"}});

/**
 * Test when hashed field is a prefix.
 */
coll.drop();

// Since the prefix field of the shard key is hashed, we pre-split the collection using hashed field
// and distribute the resulting chunks equally among the shards.
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: "hashed", b: 1, c: 1}}));

/**
 * Finds the shard to which hashed value of 'fieldValue' belongs to and validates that there exists
 * a single profiler entry on that shard, for the given 'filter'. Also verifies that no entry
 * matching 'filter' is present on the other shard.
 */
function verifyProfilerEntryOnCorrectShard(fieldValue, filter) {
    // Find the chunk to which 'hashedValue' belongs to. We use $expr here so that the $lte and $gt
    // comparisons occurs across data types.
    const hashedValue = convertShardKeyToHashed(fieldValue);
    const chunk = st.s.getDB('config').chunks.findOne({
        $expr: {
            $and: [
                {$lte: ['$min.a', hashedValue]},
                {$gt: ['$max.a', hashedValue]},
                {$eq: ['$ns', ns]}
            ]
        }
    });
    assert(chunk, st.s.getDB('config').chunks.find({ns: ns}).toArray());
    const [targetShard, otherShard] =
        (chunk.shard == st.shard0.shardName) ? [st.shard0, st.shard1] : [st.shard1, st.shard0];
    profilerHasSingleMatchingEntryOrThrow({profileDB: targetShard.getDB(kDbName), filter: filter});
    profilerHasZeroMatchingEntriesOrThrow({profileDB: otherShard.getDB(kDbName), filter: filter});
}
// Test to verify that insert operations are routed to a single shard and succeeds on the respective
// shards.
restartProfiling();
let profileFilter = {};
for (let i = -10; i < 10; i++) {
    profileFilter = {ns: ns, "command.insert": "coll", "command.documents.a": i, "ninserted": 1};
    assert.commandWorked(
        coll.insert({_id: i, a: i, b: {subObj: "str_" + (i % 5)}, c: NumberInt(i % 4)}));
    verifyProfilerEntryOnCorrectShard(i, profileFilter);
}

// Test to verify that an equality match on the hashed prefix can be routed to single shard.
testName = "FindWithEqualityOnHashedPrefix";
validateFindCmdOutputAndPlan({
    filter: {a: 0},
    expectedOutput: [{a: 0, b: {subObj: "str_0"}, c: 0}],
    expectedStages: ["IXSCAN", "FETCH", "SINGLE_SHARD"],
    testName: testName
});
profileFilter = {
    ns: ns,
    "command.find": "coll",
    "command.comment": testName
};
verifyProfilerEntryOnCorrectShard(0, profileFilter);

// Test to verify that a range query on hashed field will be routed to all nodes and the individual
// nodes cannot use index to answer the query.
testName = "FindWithRangeQueryOnHashedPrefix";
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 8}},
    expectedOutput: [{a: 9, b: {subObj: "str_4"}, c: 1}],
    expectedStages: ["COLLSCAN", "SHARD_MERGE"],
    testName: testName
});
profileFilter = {
    ns: ns,
    "command.find": "coll",
    "command.comment": testName
};
profilerHasSingleMatchingEntryOrThrow({profileDB: shard0DB, filter: profileFilter});
profilerHasSingleMatchingEntryOrThrow({profileDB: shard1DB, filter: profileFilter});

// Test to verify that update with only a shard key prefix in the query can be routed correctly.
testName = "updateWithHashedPrefix";
updateObj = {
    $set: {p: testName}
};
res = assert.commandWorked(coll.update({a: 0}, updateObj));
assert.eq(res.nModified, 1, res);

// Verify that update has modified the intended object.
assert.eq(coll.count({a: 0, p: testName}), 1);

// Verify that the update has been routed to the correct shard.
profileFilter = {
    ns: ns,
    "op": "update"
};
verifyProfilerEntryOnCorrectShard(0, profileFilter);

// Op-style update with range query on hashed field, cannot route to single shard and hence
// should fail.
assert.commandFailedWithCode(coll.update({a: {$lt: 1}}, updateObj), ErrorCodes.InvalidOptions);

// Test to verify that delete with full shard key in the query can be routed correctly.
res = assert.commandWorked(coll.deleteOne({a: 1, b: {subObj: "str_1"}, c: 1}));
assert.eq(res.deletedCount, 1, res);
assert.eq(coll.count({a: 1, b: {subObj: "str_1"}, c: 1}), 0);

profileFilter = {
    ns: ns,
    "op": "remove"
};
verifyProfilerEntryOnCorrectShard(1, profileFilter);

// Test to verify that delete with limit:1, without full shard key in query fails.
assert.commandFailedWithCode(
    coll.runCommand({delete: coll.getName(), deletes: [{q: {a: 1}, limit: 1}], ordered: false}),
    ErrorCodes.ShardKeyNotFound);

st.stop();
})();
