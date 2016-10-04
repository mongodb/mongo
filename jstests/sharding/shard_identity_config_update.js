/**
 * Tests that the config server connection string in the shard identity document of both the
 * primary and secondary will get updated whenever the config server membership changes.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    load('jstests/replsets/rslib.js');

    var st = new ShardingTest({shards: {rs0: {nodes: 2}}});

    var shardPri = st.rs0.getPrimary();

    // Note: Adding new replica set member by hand because of SERVER-24011.

    var newNode = MongoRunner.runMongod(
        {configsvr: '', replSet: st.configRS.name, storageEngine: 'wiredTiger'});

    var replConfig = st.configRS.getReplSetConfigFromNode();
    replConfig.version += 1;
    replConfig.members.push({_id: 3, host: newNode.host});

    reconfig(st.configRS, replConfig);

    /**
     * Returns true if the shardIdentity document has all the replica set member nodes in the
     * expectedConfigStr.
     */
    var checkConfigStrUpdated = function(conn, expectedConfigStr) {
        var shardIdentity = conn.getDB('admin').system.version.findOne({_id: 'shardIdentity'});

        var shardConfigsvrStr = shardIdentity.configsvrConnectionString;
        var shardConfigReplName = shardConfigsvrStr.split('/')[0];
        var expectedReplName = expectedConfigStr.split('/')[0];

        assert.eq(expectedReplName, shardConfigReplName);

        var expectedHostList = expectedConfigStr.split('/')[1].split(',');
        var shardConfigHostList = shardConfigsvrStr.split('/')[1].split(',');

        if (expectedHostList.length != shardConfigHostList.length) {
            return false;
        }

        for (var x = 0; x < expectedHostList.length; x++) {
            if (shardConfigsvrStr.indexOf(expectedHostList[x]) == -1) {
                return false;
            }
        }

        return true;
    };

    var origConfigConnStr = st.configRS.getURL();
    var expectedConfigStr = origConfigConnStr + ',' + newNode.host;
    assert.soon(function() {
        return checkConfigStrUpdated(st.rs0.getPrimary(), expectedConfigStr);
    });

    var secConn = st.rs0.getSecondary();
    secConn.setSlaveOk(true);
    assert.soon(function() {
        return checkConfigStrUpdated(secConn, expectedConfigStr);
    });

    //
    // Remove the newly added member from the config replSet while the shards are down.
    // Check that the shard identity document will be updated with the new replSet connection
    // string when they come back up.
    //

    st.rs0.stop(0);
    st.rs0.stop(1);

    MongoRunner.stopMongod(newNode.port);

    replConfig = st.configRS.getReplSetConfigFromNode();
    replConfig.version += 1;
    replConfig.members.pop();

    reconfig(st.configRS, replConfig);

    st.rs0.restart(0, {shardsvr: ''});
    st.rs0.restart(1, {shardsvr: ''});

    st.rs0.waitForMaster();
    st.rs0.awaitSecondaryNodes();

    assert.soon(function() {
        return checkConfigStrUpdated(st.rs0.getPrimary(), origConfigConnStr);
    });

    secConn = st.rs0.getSecondary();
    secConn.setSlaveOk(true);
    assert.soon(function() {
        return checkConfigStrUpdated(secConn, origConfigConnStr);
    });

    st.stop();

})();
