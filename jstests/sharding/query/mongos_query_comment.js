/**
 * Test that a legacy query via mongos retains the $comment query meta-operator when transformed
 * into a find command for the shards. In addition, verify that the find command comment parameter
 * and query operator are passed to the shards correctly, and that an attempt to attach a non-string
 * comment to the find command fails.
 */
(function() {
"use strict";

// For profilerHasSingleMatchingEntryOrThrow.
load("jstests/libs/profiler.js");

const st = new ShardingTest({name: "mongos_comment_test", mongos: 1, shards: 1});

const shard = st.shard0;
const mongos = st.s;

// Need references to the database via both mongos and mongod so that we can enable profiling &
// test queries on the shard.
const mongosDB = mongos.getDB("mongos_comment");
const shardDB = shard.getDB("mongos_comment");

assert.commandWorked(mongosDB.dropDatabase());

const mongosColl = mongosDB.test;
const shardColl = shardDB.test;

const collNS = mongosColl.getFullName();

for (let i = 0; i < 5; ++i) {
    assert.commandWorked(mongosColl.insert({_id: i, a: i}));
}

// The profiler will be used to verify that comments are present on the shard.
assert.commandWorked(shardDB.setProfilingLevel(2));
const profiler = shardDB.system.profile;

// TEST CASE: Verify that find.comment and non-string find.filter.$comment propagate.
assert.eq(mongosColl.find({a: 1, $comment: {b: "TEST"}}).comment("TEST").itcount(), 1);
profilerHasSingleMatchingEntryOrThrow({
    profileDB: shardDB,
    filter:
        {op: "query", ns: collNS, "command.comment": "TEST", "command.filter.$comment": {b: "TEST"}}
});

// TEST CASE: Verify that find command with a non-string comment parameter gets propagated.
assert.commandWorked(mongosDB.runCommand(
    {"find": mongosColl.getName(), "filter": {a: 1}, "comment": {b: "TEST_BSONOBJ"}}));

profilerHasSingleMatchingEntryOrThrow({
    profileDB: shardDB,
    filter: {op: "query", ns: collNS, "command.comment": {b: "TEST_BSONOBJ"}}
});

st.stop();
})();
