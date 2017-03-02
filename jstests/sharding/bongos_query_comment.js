/**
 * Test that a legacy query via bongos retains the $comment query meta-operator when transformed
 * into a find command for the shards. In addition, verify that the find command comment parameter
 * and query operator are passed to the shards correctly, and that an attempt to attach a non-string
 * comment to the find command fails.
 */
(function() {
    "use strict";

    // For profilerHasSingleMatchingEntryOrThrow.
    load("jstests/libs/profiler.js");

    const st = new ShardingTest({name: "bongos_comment_test", bongos: 1, shards: 1});

    const shard = st.shard0;
    const bongos = st.s;

    // Need references to the database via both bongos and bongod so that we can enable profiling &
    // test queries on the shard.
    const bongosDB = bongos.getDB("bongos_comment");
    const shardDB = shard.getDB("bongos_comment");

    assert.commandWorked(bongosDB.dropDatabase());

    const bongosColl = bongosDB.test;
    const shardColl = shardDB.test;

    const collNS = bongosColl.getFullName();

    for (let i = 0; i < 5; ++i) {
        assert.writeOK(bongosColl.insert({_id: i, a: i}));
    }

    // The profiler will be used to verify that comments are present on the shard.
    assert.commandWorked(shardDB.setProfilingLevel(2));
    const profiler = shardDB.system.profile;

    //
    // Set legacy read mode for the bongos and shard connections.
    //
    bongosDB.getBongo().forceReadMode("legacy");
    shardDB.getBongo().forceReadMode("legacy");

    // TEST CASE: A legacy string $comment meta-operator is propagated to the shards via bongos.
    assert.eq(bongosColl.find({$query: {a: 1}, $comment: "TEST"}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(shardDB,
                                          {op: "query", ns: collNS, "query.comment": "TEST"});

    // TEST CASE: A legacy BSONObj $comment is converted to a string and propagated via bongos.
    assert.eq(bongosColl.find({$query: {a: 1}, $comment: {c: 2, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB, {op: "query", ns: collNS, "query.comment": "{ c: 2.0, d: { e: \"TEST\" } }"});

    // TEST CASE: Legacy BSONObj $comment is NOT converted to a string when issued on the bongod.
    assert.eq(shardColl.find({$query: {a: 1}, $comment: {c: 3, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB, {op: "query", ns: collNS, "query.comment": {c: 3, d: {e: "TEST"}}});

    //
    // Revert to "commands" read mode for the find command test cases below.
    //
    bongosDB.getBongo().forceReadMode("commands");
    shardDB.getBongo().forceReadMode("commands");

    // TEST CASE: Verify that string find.comment and non-string find.filter.$comment propagate.
    assert.eq(bongosColl.find({a: 1, $comment: {b: "TEST"}}).comment("TEST").itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        shardDB,
        {op: "query", ns: collNS, "query.comment": "TEST", "query.filter.$comment": {b: "TEST"}});

    // TEST CASE: Verify that find command with a non-string comment parameter is rejected.
    assert.commandFailedWithCode(
        bongosDB.runCommand(
            {"find": bongosColl.getName(), "filter": {a: 1}, "comment": {b: "TEST"}}),
        9,
        "Non-string find command comment did not return an error.");

    st.stop();
})();