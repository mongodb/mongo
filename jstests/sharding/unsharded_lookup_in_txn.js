/**
 * Test that $lookup within a sharded transaction reads from the correct snapshot.
 * @tags: [requires_find_command, requires_sharding, uses_multi_shard_transaction,
 *         uses_transactions]
 */
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, mongos: 1});
    const kDBName = "unsharded_lookup_in_txn";

    let session = st.s.startSession();
    let sessionDB = session.getDatabase("unsharded_lookup_in_txn");

    const shardedColl = sessionDB.sharded;
    const unshardedColl = sessionDB.unsharded;

    assert.commandWorked(st.s.adminCommand({enableSharding: sessionDB.getName()}));
    st.ensurePrimaryShard(sessionDB.getName(), st.shard0.shardName);

    assert.commandWorked(
        st.s.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

    // Move all of the data to shard 1.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: shardedColl.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

    // Insert a bunch of documents, all of which reside on the same chunk (on shard 1).
    for (let i = -10; i < 10; i++) {
        assert.commandWorked(shardedColl.insert({_id: i, local_always_one: 1}));
    }

    const pipeline = [{
        $lookup: {
            from: unshardedColl.getName(),
            localField: "local_always_one",
            foreignField: "foreign_always_one",
            as: "matches"
        }
    }];
    const kBatchSize = 2;

    const testLookupDoesNotSeeDocumentsOutsideSnapshot = function() {
        unshardedColl.drop();
        // Insert some stuff into the unsharded collection.
        const kUnshardedCollOriginalSize = 10;
        for (let i = 0; i < kUnshardedCollOriginalSize; i++) {
            assert.commandWorked(unshardedColl.insert({_id: i, foreign_always_one: 1}));
        }

        session.startTransaction();

        const curs = shardedColl.aggregate(
            pipeline, {readConcern: {level: "snapshot"}, cursor: {batchSize: kBatchSize}});

        for (let i = 0; i < kBatchSize; i++) {
            const doc = curs.next();
            assert.eq(doc.matches.length, kUnshardedCollOriginalSize);
        }

        // Do writes on the unsharded collection from outside the session.
        (function() {
            const unshardedCollOutsideSession =
                st.s.getDB(sessionDB.getName())[unshardedColl.getName()];
            assert.commandWorked(unshardedCollOutsideSession.insert({b: 1, xyz: 1}));
            assert.commandWorked(unshardedCollOutsideSession.insert({b: 1, xyz: 2}));
        })();

        // We shouldn't see those writes from the aggregation within the session.
        assert.eq(curs.hasNext(), true);
        while (curs.hasNext()) {
            const doc = curs.next();
            assert.eq(doc.matches.length, kUnshardedCollOriginalSize);
        }

        session.abortTransaction();
    };

    // Run the test once, with all of the data on shard 1. This means that the merging shard (shard
    // 0) will not be targeted. This is interesting because in contrast to the case below, the
    // merging half of the pipeline will start the transaction on the merging shard.
    testLookupDoesNotSeeDocumentsOutsideSnapshot();

    // Move some data to shard 0, so that the merging shard will be targeted.
    assert.commandWorked(st.s.adminCommand({split: shardedColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: shardedColl.getFullName(), find: {_id: -1}, to: st.shard0.shardName}));

    // Run the test again.
    testLookupDoesNotSeeDocumentsOutsideSnapshot();

    st.stop();
})();
