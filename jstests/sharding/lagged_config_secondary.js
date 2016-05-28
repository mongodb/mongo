/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 */
(function() {
    var st = new ShardingTest({shards: 1});

    var configSecondaryList = st.configRS.getSecondaries();
    var configSecondaryToKill = configSecondaryList[0];
    var delayedConfigSecondary = configSecondaryList[1];

    delayedConfigSecondary.getDB('admin').adminCommand(
        {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

    var testDB = st.s.getDB('test');
    testDB.adminCommand({enableSharding: 'test'});
    testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}});

    testDB.user.insert({_id: 1});

    st.configRS.stopMaster();
    MongoRunner.stopMongod(configSecondaryToKill.port);

    // Clears all cached info so mongos will be forced to query from the config.
    st.s.adminCommand({flushRouterConfig: 1});

    var exception = assert.throws(function() {
        testDB.user.findOne();
    });

    assert.eq(ErrorCodes.ExceededTimeLimit, exception.code);

    var msg = 'Command on database config timed out waiting for read concern to be satisfied.';
    assert.soon(function() {
        var logMessages =
            assert.commandWorked(delayedConfigSecondary.adminCommand({getLog: 'global'})).log;
        for (var i = 0; i < logMessages.length; i++) {
            if (logMessages[i].indexOf(msg) != -1) {
                return true;
            }
        }
        return false;
    }, 'Did not see any log entries containing the following message: ' + msg, 60000, 300);

    st.stop();

}());
