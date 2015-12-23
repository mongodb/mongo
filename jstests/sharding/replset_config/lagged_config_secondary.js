/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 */
(function(){
var st = new ShardingTest({ shards: 1 });

var configSecondaryList = st.configRS.getSecondaries();
var configSecondaryToKill = configSecondaryList[0];
var delayedConfigSecondary = configSecondaryList[1];

delayedConfigSecondary.getDB('admin').adminCommand({ configureFailPoint: 'rsSyncApplyStop',
                                                     mode: 'alwaysOn' });

var testDB = st.s.getDB('test');
testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ shardCollection: 'test.user', key: { _id: 1 }});

testDB.user.insert({ _id: 1 });

st.configRS.stopMaster();
MongoRunner.stopMongod(configSecondaryToKill.port);

// Clears all cached info so mongos will be forced to query from the config.
st.s.adminCommand({ flushRouterConfig: 1 });

var exception = assert.throws(function() {
    testDB.user.findOne();
});

assert.eq(ErrorCodes.ExceededTimeLimit, exception.code);

st.stop();

}());
