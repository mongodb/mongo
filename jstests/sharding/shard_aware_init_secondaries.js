/**
 * Tests for shard aware initialization on secondaries during startup and shard
 * identity document creation.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet({shardsvr: ''});
    var nodeList = replTest.nodeList();
    replTest.initiate({
        _id: replTest.name,
        members:
            [{_id: 0, host: nodeList[0], priority: 1}, {_id: 1, host: nodeList[1], priority: 0}]
    });

    var priConn = replTest.getPrimary();
    var configConnStr = st.configRS.getURL();

    var shardIdentityDoc = {
        _id: 'shardIdentity',
        configsvrConnectionString: configConnStr,
        shardName: 'newShard',
        clusterId: ObjectId()
    };

    // Simulate the upsert that is performed by a config server on addShard.
    var shardIdentityQuery = {
        _id: shardIdentityDoc._id,
        shardName: shardIdentityDoc.shardName,
        clusterId: shardIdentityDoc.clusterId
    };
    var shardIdentityUpdate = {
        $set: {configsvrConnectionString: shardIdentityDoc.configsvrConnectionString}
    };
    assert.writeOK(priConn.getDB('admin').system.version.update(
        shardIdentityQuery, shardIdentityUpdate, {upsert: true, writeConcern: {w: 2}}));

    var secConn = replTest.getSecondary();
    secConn.setSlaveOk(true);

    var res = secConn.getDB('admin').runCommand({shardingState: 1});

    assert(res.enabled, tojson(res));
    assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);

    var newMongodOptions = Object.extend(secConn.savedOptions, {restart: true});
    replTest.restart(replTest.getNodeId(secConn), newMongodOptions);
    replTest.waitForMaster();
    replTest.awaitSecondaryNodes();

    secConn = replTest.getSecondary();
    secConn.setSlaveOk(true);

    res = secConn.getDB('admin').runCommand({shardingState: 1});

    assert(res.enabled, tojson(res));
    assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);

    replTest.stopSet();

    st.stop();
})();
