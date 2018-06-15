// Test that a $changeStream pipeline on a sharded cluster always enforces the user-specified
// maxTimeMS on mongoS, but caps the maxTimeMS of getMores sent to the shards at one second. Doing
// so allows the shards to regularly report their advancing optimes in the absence of any new data,
// which in turn allows the AsyncResultsMerger to return sorted results retrieved from the other
// shards.
// @tags: [requires_majority_read_concern]
(function() {
    "use strict";

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // This test only works on storage engines that support committed reads, skip it if the
    // configured engine doesn't support it.
    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Create a 2-shard cluster. Enable 'writePeriodicNoops' and set 'periodicNoopIntervalSecs' to 1
    // second so that each shard is continually advancing its optime, allowing the
    // AsyncResultsMerger to return sorted results even if some shards have not yet produced any
    // data.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}}
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    const shard0DB = st.shard0.getDB(jsTestName());
    const shard1DB = st.shard1.getDB(jsTestName());

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey] chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Start the profiler on each shard so that we can examine the getMores' maxTimeMS.
    for (let profileDB of[shard0DB, shard1DB]) {
        assert.commandWorked(profileDB.setProfilingLevel(0));
        profileDB.system.profile.drop();
        assert.commandWorked(profileDB.setProfilingLevel(2));
    }

    // Returns 'true' if there is at least one getMore profile entry matching the given namespace,
    // identifying comment and maxTimeMS.
    function profilerHasAtLeastOneMatchingGetMore(profileDB, nss, comment, timeout) {
        return profileDB.system.profile.count({
            "originatingCommand.comment": comment,
            "command.maxTimeMS": timeout,
            op: "getmore",
            ns: nss
        }) > 0;
    }

    // Asserts that there is at least one getMore profile entry matching the given namespace and
    // identifying comment, and that all such entries have the given maxTimeMS.
    function assertAllGetMoresHaveTimeout(profileDB, nss, comment, timeout) {
        const getMoreTimeouts =
            profileDB.system.profile
                .aggregate([
                    {$match: {op: "getmore", ns: nss, "originatingCommand.comment": comment}},
                    {$group: {_id: "$command.maxTimeMS"}}
                ])
                .toArray();
        assert.eq(getMoreTimeouts.length, 1);
        assert.eq(getMoreTimeouts[0]._id, timeout);
    }

    // Kills the cursor with the given cursor id (if provided). Then opens a new change stream
    // against 'mongosColl' and returns the new change stream's cursor id.
    //
    // We re-open the change stream in between each test case with a batchSize if 0. This is done to
    // ensure that mongos delivers getMores to the shards for the first getMore against the mongos
    // change stream cursor (thus avoiding issues such as SERVER-35084).
    function reopenChangeStream(existingCursorId) {
        if (existingCursorId) {
            assert.commandWorked(mongosDB.runCommand(
                {killCursors: mongosColl.getName(), cursors: [existingCursorId]}));
        }

        const csCmdRes = assert.commandWorked(mongosDB.runCommand({
            aggregate: mongosColl.getName(),
            pipeline: [{$changeStream: {}}],
            comment: testComment,
            cursor: {batchSize: 0}
        }));
        assert.eq(csCmdRes.cursor.firstBatch.length, 0);
        assert.neq(csCmdRes.cursor.id, 0);
        return csCmdRes.cursor.id;
    }

    // Timeout values used in the subsequent getMore tests.
    const quarterSec = 250;
    const halfSec = 500;
    const oneSec = 2 * halfSec;
    const fiveSecs = 5 * oneSec;
    const fiveMins = 60 * fiveSecs;
    const thirtyMins = 6 * fiveMins;
    const testComment = "change stream sharded maxTimeMS test";

    // Open a $changeStream on the empty, inactive collection.
    let csCursorId = reopenChangeStream();

    // Confirm that getMores without an explicit maxTimeMS default to one second on the shards.
    assert.commandWorked(
        mongosDB.runCommand({getMore: csCursorId, collection: mongosColl.getName()}));
    for (let shardDB of[shard0DB, shard1DB]) {
        // The mongos is guaranteed to have already delivered getMores to each of the shards.
        // However, the mongos await time can expire prior to the await time on the shards.
        // Therefore, the getMore on mongos succeeding doesn't guarantee that the getMores on the
        // shards have already been profiled. We use an assert.soon() here to wait for the maxTimeMS
        // on the shards to expire, at which point the getMores will appear in the profile
        // collection.
        assert.soon(() => profilerHasAtLeastOneMatchingGetMore(
                        shardDB, mongosColl.getFullName(), testComment, oneSec));
    }

    // Verify that with no activity on the shards, a $changeStream with maxTimeMS waits for the full
    // duration on mongoS. Allow some leniency since the server-side wait may wake spuriously.
    csCursorId = reopenChangeStream(csCursorId);
    let startTime = (new Date()).getTime();
    assert.commandWorked(mongosDB.runCommand(
        {getMore: csCursorId, collection: mongosColl.getName(), maxTimeMS: fiveSecs}));
    assert.gte((new Date()).getTime() - startTime, fiveSecs - halfSec);

    // Confirm that each getMore dispatched to the shards during this period had a maxTimeMS of 1s.
    for (let shardDB of[shard0DB, shard1DB]) {
        assertAllGetMoresHaveTimeout(shardDB, mongosColl.getFullName(), testComment, oneSec);
    }

    // Issue a getMore with a sub-second maxTimeMS. This should propagate to the shards as-is.
    csCursorId = reopenChangeStream(csCursorId);
    assert.commandWorked(mongosDB.runCommand(
        {getMore: csCursorId, collection: mongosColl.getName(), maxTimeMS: halfSec}));

    for (let shardDB of[shard0DB, shard1DB]) {
        // The mongos is guaranteed to have already delivered getMores to each of the shards.
        // However, the mongos await time can expire prior to the await time on the shards.
        // Therefore, the getMore on mongos succeeding doesn't guarantee that the getMores on the
        // shards have already been profiled. We use an assert.soon() here to wait for the maxTimeMS
        // on the shards to expire, at which point the getMores will appear in the profile
        // collection.
        assert.soon(() => profilerHasAtLeastOneMatchingGetMore(
                        shardDB, mongosColl.getFullName(), testComment, halfSec));
    }

    // Write a document to shard0, and confirm that - despite the fact that shard1 is still idle - a
    // getMore with a high maxTimeMS returns the document before this timeout expires.
    csCursorId = reopenChangeStream(csCursorId);
    assert.writeOK(mongosColl.insert({_id: -1}));
    startTime = (new Date()).getTime();
    const csResult = assert.commandWorked(mongosDB.runCommand(
        {getMore: csCursorId, collection: mongosColl.getName(), maxTimeMS: thirtyMins}));
    assert.lte((new Date()).getTime() - startTime, fiveMins);
    assert.docEq(csResult.cursor.nextBatch[0].fullDocument, {_id: -1});

    // Open a change stream with the default maxTimeMS. Then verify that if the client starts
    // issuing getMores with a subsecond maxTimeMS, that mongos eventually schedules getMores on the
    // shards with this subsecond maxTimeMS value.
    csCursorId = reopenChangeStream(csCursorId);
    assert.commandWorked(
        mongosDB.runCommand({getMore: csCursorId, collection: mongosColl.getName()}));
    assert.soon(function() {
        // Run a getMore with a 250ms maxTimeMS against mongos.
        assert.commandWorked(mongosDB.runCommand(
            {getMore: csCursorId, collection: mongosColl.getName(), maxTimeMS: quarterSec}));
        // Check whether all shards now have a getMore with 250ms maxTimeMS recorded in their
        // profile collections.
        return [shard0DB, shard1DB].every(function(shardDB) {
            return profilerHasAtLeastOneMatchingGetMore(
                shardDB, mongosColl.getFullName(), testComment, quarterSec);
        });
    });

    st.stop();
})();
