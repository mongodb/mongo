/**
 * Tests that the cluster is balanced in parallel in one balancer round (standalone).
 */

(function() {
    'use strict';

    var st = new ShardingTest({shards: 4});
    var config = st.s0.getDB('config');

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);

    function prepareCollectionForBalance(collName) {
        assert.commandWorked(st.s0.adminCommand({shardCollection: collName, key: {Key: 1}}));

        var coll = st.s0.getCollection(collName);

        // Create 4 chunks initially and ensure they get balanced within 1 balancer round
        assert.writeOK(coll.insert({Key: 1, Value: 'Test value 1'}));
        assert.writeOK(coll.insert({Key: 10, Value: 'Test value 10'}));
        assert.writeOK(coll.insert({Key: 20, Value: 'Test value 20'}));
        assert.writeOK(coll.insert({Key: 30, Value: 'Test value 30'}));

        assert.commandWorked(st.splitAt(collName, {Key: 10}));
        assert.commandWorked(st.splitAt(collName, {Key: 20}));
        assert.commandWorked(st.splitAt(collName, {Key: 30}));

        // Move two of the chunks to shard0001 so we have option to do parallel balancing
        assert.commandWorked(st.moveChunk(collName, {Key: 20}, st.shard1.shardName));
        assert.commandWorked(st.moveChunk(collName, {Key: 30}, st.shard1.shardName));

        assert.eq(2, config.chunks.find({ns: collName, shard: st.shard0.shardName}).itcount());
        assert.eq(2, config.chunks.find({ns: collName, shard: st.shard1.shardName}).itcount());
    }

    function checkCollectionBalanced(collName) {
        assert.eq(1, config.chunks.find({ns: collName, shard: st.shard0.shardName}).itcount());
        assert.eq(1, config.chunks.find({ns: collName, shard: st.shard1.shardName}).itcount());
        assert.eq(1, config.chunks.find({ns: collName, shard: st.shard2.shardName}).itcount());
        assert.eq(1, config.chunks.find({ns: collName, shard: st.shard3.shardName}).itcount());
    }

    function countMoves(collName) {
        return config.changelog.find({what: 'moveChunk.start', ns: collName}).itcount();
    }

    prepareCollectionForBalance('TestDB.TestColl1');
    prepareCollectionForBalance('TestDB.TestColl2');

    // Count the moveChunk start attempts accurately and ensure that only the correct number of
    // migrations are scheduled
    const testColl1InitialMoves = countMoves('TestDB.TestColl1');
    const testColl2InitialMoves = countMoves('TestDB.TestColl2');

    st.startBalancer();
    st.awaitBalancerRound();
    st.awaitBalancerRound();
    st.stopBalancer();

    checkCollectionBalanced('TestDB.TestColl1');
    checkCollectionBalanced('TestDB.TestColl2');

    assert.eq(2, countMoves('TestDB.TestColl1') - testColl1InitialMoves);
    assert.eq(2, countMoves('TestDB.TestColl2') - testColl2InitialMoves);

    // Ensure there are no migration errors reported
    assert.eq(0, config.changelog.find({what: 'moveChunk.error'}).itcount());

    st.stop();
})();
