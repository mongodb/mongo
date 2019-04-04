/**
 * Test that a legacy query via merizos retains the $comment query meta-operator when transformed
 * into a find command for the shards. In addition, verify that the find command comment parameter
 * and query operator are passed to the shards correctly, and that an attempt to attach a non-string
 * comment to the find command fails.
 */
(function() {
    "use strict";

    // For profilerHasSingleMatchingEntryOrThrow.
    load("jstests/libs/profiler.js");

    const st = new ShardingTest({name: "merizos_comment_test", merizos: 1, shards: 1});

    const shard = st.shard0;
    const merizos = st.s;

    // Need references to the database via both merizos and merizod so that we can enable profiling &
    // test queries on the shard.
    const merizosDB = merizos.getDB("merizos_comment");
    const shardDB = shard.getDB("merizos_comment");

    assert.commandWorked(merizosDB.dropDatabase());

    const merizosColl = merizosDB.test;
    const shardColl = shardDB.test;

    const collNS = merizosColl.getFullName();

    for (let i = 0; i < 5; ++i) {
        assert.writeOK(merizosColl.insert({_id: i, a: i}));
    }

    // The profiler will be used to verify that comments are present on the shard.
    assert.commandWorked(shardDB.setProfilingLevel(2));
    const profiler = shardDB.system.profile;

    //
    // Set legacy read mode for the merizos and shard connections.
    //
    merizosDB.getMerizo().forceReadMode("legacy");
    shardDB.getMerizo().forceReadMode("legacy");

    // TEST CASE: A legacy string $comment meta-operator is propagated to the shards via merizos.
    assert.eq(merizosColl.find({$query: {a: 1}, $comment: "TEST"}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow(
        {profileDB: shardDB, filter: {op: "query", ns: collNS, "command.comment": "TEST"}});

    // TEST CASE: A legacy BSONObj $comment is converted to a string and propagated via merizos.
    assert.eq(merizosColl.find({$query: {a: 1}, $comment: {c: 2, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardDB,
        filter: {op: "query", ns: collNS, "command.comment": "{ c: 2.0, d: { e: \"TEST\" } }"}
    });

    // TEST CASE: Legacy BSONObj $comment is NOT converted to a string when issued on the merizod.
    assert.eq(shardColl.find({$query: {a: 1}, $comment: {c: 3, d: {e: "TEST"}}}).itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardDB,
        filter: {op: "query", ns: collNS, "command.comment": {c: 3, d: {e: "TEST"}}}
    });

    //
    // Revert to "commands" read mode for the find command test cases below.
    //
    merizosDB.getMerizo().forceReadMode("commands");
    shardDB.getMerizo().forceReadMode("commands");

    // TEST CASE: Verify that string find.comment and non-string find.filter.$comment propagate.
    assert.eq(merizosColl.find({a: 1, $comment: {b: "TEST"}}).comment("TEST").itcount(), 1);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shardDB,
        filter: {
            op: "query",
            ns: collNS, "command.comment": "TEST", "command.filter.$comment": {b: "TEST"}
        }
    });

    // TEST CASE: Verify that find command with a non-string comment parameter is rejected.
    assert.commandFailedWithCode(
        merizosDB.runCommand(
            {"find": merizosColl.getName(), "filter": {a: 1}, "comment": {b: "TEST"}}),
        9,
        "Non-string find command comment did not return an error.");

    st.stop();
})();