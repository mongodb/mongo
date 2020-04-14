/**
 * Tests that after a restart of a shard, multi write operations, finds and aggregations still work
 * as expected with a stale router
 *
 * This test requrires persistence because it asumes the shard will still have it's data after
 * restarting
 *
 * @tags: [requires_persistence]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2, mongos: 2});

// Used to get the shard destination ids for the moveChunks commands
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;

const kDatabaseName = 'TestDB';
st.enableSharding(kDatabaseName, st.shard1.shardName);

// Creates and shard collName with 2 chunks, one per shard. Only the router referenced by st.s0
// knows that collName is sharded, and all the shards are restarted so they don't have the
// collection's sharding status
function setupCollectionForTest(collName) {
    const ns = kDatabaseName + '.' + collName;
    assert.commandFailedWithCode(st.s0.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);
    st.shardCollection(ns, {Key: 1});

    st.s0.adminCommand({split: ns, middle: {Key: 0}});
    st.s0.adminCommand({moveChunk: ns, find: {Key: -1}, to: shard0Name});
    st.s0.adminCommand({moveChunk: ns, find: {Key: 0}, to: shard1Name});
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}),
                                 ErrorCodes.NamespaceNotSharded);

    // This document will go to shard 0
    assert.commandWorked(
        st.s0.getDB(kDatabaseName).getCollection(collName).insert({Key: -1, inc: 0}));
    // This document will go to shard 1
    assert.commandWorked(
        st.s0.getDB(kDatabaseName).getCollection(collName).insert({Key: 0, inc: 0}));

    st.restartShardRS(0);
    st.restartShardRS(1);
}

const freshMongoS = st.s0;
const staleMongoS = st.s1;

{
    jsTest.log('Testing: Insert with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestInsertColl');

    var insertBulkOp = staleMongoS.getDB(kDatabaseName).TestInsertColl.initializeUnorderedBulkOp();
    insertBulkOp.insert({Key: -2});
    insertBulkOp.insert({Key: 1});
    insertBulkOp.execute();

    assert.eq(4, freshMongoS.getDB(kDatabaseName).TestInsertColl.find().itcount());
    assert.eq(4, staleMongoS.getDB(kDatabaseName).TestInsertColl.find().itcount());
}
{
    jsTest.log('Testing: Multi-update with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestUpdateColl');

    assert.commandWorked(staleMongoS.getDB(kDatabaseName)
                             .TestUpdateColl.update({}, {$inc: {inc: 1}}, {multi: true}));

    var s0Doc = freshMongoS.getDB(kDatabaseName).TestUpdateColl.findOne({Key: -1});
    assert.eq(1, s0Doc.inc);
    var s1Doc = freshMongoS.getDB(kDatabaseName).TestUpdateColl.findOne({Key: 0});
    assert.eq(1, s1Doc.inc);
}
{
    jsTest.log('Testing: Multi-remove with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestRemoveColl');

    assert.commandWorked(
        staleMongoS.getDB(kDatabaseName).TestRemoveColl.remove({}, {justOne: false}));

    assert.eq(0, freshMongoS.getDB(kDatabaseName).TestRemoveColl.find().itcount());
}
{
    jsTest.log('Testing: Find-and-modify with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestFindAndModifyColl');

    assert.eq(null, staleMongoS.getDB(kDatabaseName).TestFindAndModifyColl.findAndModify({
        query: {Key: -2},
        update: {Key: -2},
        upsert: true
    }));

    assert.eq({Key: -2},
              freshMongoS.getDB(kDatabaseName).TestFindAndModifyColl.findOne({Key: -2}, {_id: 0}));
}
{
    jsTest.log('Testing: Find with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestFindColl');

    var coll = staleMongoS.getDB(kDatabaseName).TestFindColl.find().toArray();
    assert.eq(2, coll.length);
}
{
    jsTest.log('Testing: Aggregate with sharded collection unknown on a stale mongos');
    setupCollectionForTest('TestAggregateColl');

    var count =
        staleMongoS.getDB(kDatabaseName).TestAggregateColl.aggregate([{$count: 'total'}]).toArray();
    assert.eq(2, count[0].total);
}
{
    jsTest.log('Testing: Transactions with unsharded collection, which is unknown on the shard');
    st.restartShardRS(0);
    st.restartShardRS(1);

    var session = staleMongoS.startSession();
    session.startTransaction();
    session.getDatabase(kDatabaseName).TestTransactionColl.insert({Key: 1});
    session.commitTransaction();
}

st.stop();
})();
