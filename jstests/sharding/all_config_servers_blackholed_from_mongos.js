// Ensures that if the config servers are blackholed from the point of view of MongoS, metadata
// operations do not get stuck forever.
(function() {
    'use strict';

    var st = new ShardingTest({
        name: 'all_config_servers_blackholed_from_mongos',
        shards: 2,
        mongos: 1,
        useBridge: true,
    });

    var testDB = st.s.getDB('BlackHoleDB');

    assert.commandWorked(testDB.adminCommand({enableSharding: 'BlackHoleDB'}));
    assert.commandWorked(
        testDB.adminCommand({shardCollection: testDB.ShardedColl.getFullName(), key: {_id: 1}}));

    assert.writeOK(testDB.ShardedColl.insert({a: 1}));

    jsTest.log('Making all the config servers appear as a blackhole to mongos');
    st._configServers.forEach(function(configSvr) {
        configSvr.discardMessagesFrom(st.s, 1.0);
    });

    assert.commandWorked(testDB.adminCommand({flushRouterConfig: 1}));

    // This shouldn't stall
    jsTest.log('Doing read operation on the sharded collection');
    assert.throws(function() {
        testDB.ShardedColl.find({}).itcount();
    });

    // This should fail, because the primary is not available
    jsTest.log('Doing write operation on a new database and collection');
    assert.writeError(st.s.getDB('NonExistentDB')
                          .TestColl.insert({_id: 0, value: 'This value will never be inserted'}));

    st.stop();

}());
