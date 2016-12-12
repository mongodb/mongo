/**
 * Test that a new primary that gets elected will properly perform shard initialization.
 */

(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    var replTest = new ReplSetTest({nodes: 3});
    replTest.startSet({shardsvr: ''});
    var nodes = replTest.nodeList();
    replTest.initiate({
        _id: replTest.name,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1]},
            {_id: 2, host: nodes[2], arbiterOnly: true}
        ],
        protocolVersion: 1
    });

    var primaryConn = replTest.getPrimary();

    var shardIdentityDoc = {
        _id: 'shardIdentity',
        configsvrConnectionString: st.configRS.getURL(),
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
    assert.writeOK(primaryConn.getDB('admin').system.version.update(
        shardIdentityQuery, shardIdentityUpdate, {upsert: true, writeConcern: {w: 'majority'}}));

    replTest.stopMaster();
    replTest.waitForMaster();

    primaryConn = replTest.getPrimary();

    var res = primaryConn.getDB('admin').runCommand({shardingState: 1});

    assert(res.enabled);
    assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);

    replTest.stopSet();

    st.stop();

})();
