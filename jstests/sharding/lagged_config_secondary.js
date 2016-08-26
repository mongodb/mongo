/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 */
(function() {
    var st = new ShardingTest(
        {shards: 1, configReplSetTestOptions: {settings: {chainingAllowed: false}}});
    var testDB = st.s.getDB('test');

    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));

    // Ensures that all metadata writes thus far have been replicated to all nodes
    st.configRS.awaitReplication();

    var configSecondaryList = st.configRS.getSecondaries();
    var configSecondaryToKill = configSecondaryList[0];
    var delayedConfigSecondary = configSecondaryList[1];

    delayedConfigSecondary.getDB('admin').adminCommand(
        {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

    assert.writeOK(testDB.user.insert({_id: 1}));

    // Do one metadata write in order to bump the optime on mongos
    assert.writeOK(st.getDB('config').TestConfigColl.insert({TestKey: 'Test value'}));

    st.configRS.stopMaster();
    MongoRunner.stopMongod(configSecondaryToKill.port);

    // Clears all cached info so mongos will be forced to query from the config.
    st.s.adminCommand({flushRouterConfig: 1});

    print('Attempting read on a sharded collection...');
    var exception = assert.throws(function() {
        testDB.user.find({}).maxTimeMS(15000).itcount();
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

    // Can't do clean shutdown with this failpoint on.
    delayedConfigSecondary.getDB('admin').adminCommand(
        {configureFailPoint: 'rsSyncApplyStop', mode: 'off'});

    st.stop();
}());
