/**
 * Test that adding invalid or duplicate shards will fail.
 */
(function() {

    "use strict";

    var st = new ShardingTest({shards: 1});

    var configDB = st.s.getDB('config');
    var shardDoc = configDB.shards.findOne();

    // Can't add mongos as shard.
    assert.commandFailed(st.admin.runCommand({addshard: st.s.host}));

    // Can't add config servers as shard.
    assert.commandFailed(st.admin.runCommand({addshard: st._configDB}));

    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet({oplogSize: 10});
    replTest.initiate();

    var rsConnStr = replTest.getURL();
    // Can't add replSet as shard if the name doesn't match the replSet config.
    assert.commandFailed(st.admin.runCommand({addshard: "prefix_" + rsConnStr}));

    assert.commandWorked(st.admin.runCommand({addshard: rsConnStr, name: 'dummyRS'}));

    // Cannot add the same replSet shard host twice when using a unique shard name.
    assert.commandFailed(st.admin.runCommand({addshard: rsConnStr, name: 'dupRS'}));

    // Cannot add the same stand alone shard host twice with a unique shard name.
    assert.commandFailed(st.admin.runCommand({addshard: shardDoc.host, name: 'dupShard'}));

    // Cannot add a replica set connection string containing a member that isn't actually part of
    // the replica set.
    var truncatedRSConnStr = rsConnStr.substring(0, rsConnStr.indexOf(','));
    assert.commandFailed(
        st.admin.runCommand({addshard: truncatedRSConnStr + 'fakehost', name: 'dummyRS'}));

    replTest.stopSet();
    st.stop();

})();
