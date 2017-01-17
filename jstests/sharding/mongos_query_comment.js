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
        assert.writeOK(mongosColl.insert({_id: i, a: i}));
    }

    // The profiler will be used to verify that comments are present on the shard.
    assert.commandWorked(shardDB.setProfilingLevel(2));
    const profiler = shardDB.system.profile;

    //
    // Set legacy read mode for the mongos and shard connections.
    //
    mongosDB.getMongo().forceReadMode("legacy");
    shardDB.getMongo().forceReadMode("legacy");

    // TEST CASE: A legacy string $comment meta-operator is propagated to the shards via mongos.
    assert.eq(mongosColl.find({$query: {a: 1}, $comment: "TEST"}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(shardDB,
                                          {op: "query", ns: collNS, "query.comment": "TEST"});

    // TEST CASE: A legacy BSONObj $comment is converted to a string and propagated via mongos.
    assert.eq(mongosColl.find({$query: {a: 1}, $comment: {c: 2, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB, {op: "query", ns: collNS, "query.comment": "{ c: 2.0, d: { e: \"TEST\" } }"});

    // TEST CASE: Legacy BSONObj $comment is NOT converted to a string when issued on the mongod.
    assert.eq(shardColl.find({$query: {a: 1}, $comment: {c: 3, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB, {op: "query", ns: collNS, "query.comment": {c: 3, d: {e: "TEST"}}});

    //
    // Revert to "commands" read mode for the find command test cases below.
    //
    mongosDB.getMongo().forceReadMode("commands");
    shardDB.getMongo().forceReadMode("commands");

    // TEST CASE: Verify that string find.comment and non-string find.filter.$comment propagate.
    assert.eq(mongosColl.find({a: 1, $comment: {b: "TEST"}}).comment("TEST").itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB,
        {op: "query", ns: collNS, "query.comment": "TEST", "query.filter.$comment": {b: "TEST"}});

    // TEST CASE: Verify that find command with a non-string comment parameter is rejected.
    assert.commandFailedWithCode(
        mongosDB.runCommand(
            {"find": mongosColl.getName(), "filter": {a: 1}, "comment": {b: "TEST"}}),
        9,
        "Non-string find command comment did not return an error.");

    st.stop();
})();