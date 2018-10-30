// @tags: [uses_transactions, requires_find_command, uses_multi_shard_transaction]
(function() {

    "use strict";

    const st = new ShardingTest({shards: 2});

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard("test", st.shard0.shardName);

    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {_id: 0}, to: st.shard1.shardName}));

    // Preemptively create the collections in the shard since it is not allowed in transactions.
    let coll = st.s.getDB('test').user;
    coll.insert({_id: 1});
    coll.insert({_id: -1});
    coll.remove({});

    let unshardedColl = st.s.getDB('test').foo;
    unshardedColl.insert({_id: 0});
    unshardedColl.remove({});

    let session = st.s.startSession();
    let sessionDB = session.getDatabase('test');
    let sessionColl = sessionDB.getCollection('user');
    let sessionUnsharded = sessionDB.getCollection('foo');

    // passthrough

    session.startTransaction();

    sessionUnsharded.insert({_id: -1});
    sessionUnsharded.insert({_id: 1});
    assert.eq(2, sessionUnsharded.find().itcount());

    let res = sessionUnsharded.aggregate([{$match: {_id: {$gte: -200}}}]).toArray();
    assert.eq(2, res.length, tojson(res));

    session.abortTransaction_forTesting();

    // merge on mongos

    session.startTransaction();

    sessionColl.insert({_id: -1});
    sessionColl.insert({_id: 1});
    assert.eq(2, sessionColl.find().itcount());

    res = sessionColl.aggregate([{$match: {_id: {$gte: -200}}}], {allowDiskUse: false}).toArray();
    assert.eq(2, res.length, tojson(res));

    session.abortTransaction_forTesting();

    // merge on shard
    // TODO: SERVER-33683 uncomment test after fixing deadlock.
    /*
        session.startTransaction();

        sessionColl.insert({_id: -1});
        sessionColl.insert({_id: 1});
        assert.eq(2, sessionColl.find().itcount());

        res =
            sessionColl
                .aggregate(
                    [{$match: {_id: {$gte: -200}}}, {$_internalSplitPipeline: {mergeType:
       "anyShard"}}])
                .toArray();
        assert.eq(2, res.length, tojson(res));

        session.abortTransaction();
    */
    st.stop();

})();
