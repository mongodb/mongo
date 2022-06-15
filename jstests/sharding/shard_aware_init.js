/**
 * Tests for shard aware initialization during process startup (for standalone) and transition
 * to primary (for replica set nodes).
 * Note: test will deliberately cause a mongod instance to terminate abruptly and mongod instance
 * without journaling will complain about unclean shutdown.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

var waitForPrimary = function(conn) {
    assert.soon(function() {
        var res = conn.getDB('admin').runCommand({hello: 1});
        return res.isWritablePrimary;
    });
};

/**
 * Runs a series of test on the mongod instance mongodConn is pointing to. Notes that the
 * test can restart the mongod instance several times so mongodConn can end up with a broken
 * connection after.
 *
 * awaitVersionUpdate is used with the replset invocation of this test to ensure that our
 * initial write to the admin.system.version collection is fully flushed out of the oplog before
 * restarting.  That allows our standalone corrupting update to see the write (and cause us to
 * fail on startup).
 *
 * TODO: Remove awaitVersionUpdate after SERVER-41005, where we figure out how to wait until
 *       after replication is started before reading our shard identity from
 *       admin.system.version
 */
var runTest = function(mongodConn, configConnStr, awaitVersionUpdate) {
    var shardIdentityDoc = {
        _id: 'shardIdentity',
        configsvrConnectionString: configConnStr,
        shardName: 'newShard',
        clusterId: ObjectId()
    };

    /**
     * Restarts the server without --shardsvr and replace the shardIdentity doc with a valid
     * document. Then, restarts the server again with --shardsvr. This also returns a
     * connection to the server after the last restart.
     */
    var restartAndFixShardIdentityDoc = function(startOptions) {
        var options = Object.extend({}, startOptions);
        // With Recover to a Timestamp, writes to a replica set member may not be written to
        // disk in the collection, but are instead re-applied from the oplog at startup. When
        // restarting with `--shardsvr`, the update to the `shardIdentity` document is not
        // processed. Turning off `--replSet` guarantees the update is written out to the
        // collection and the test no longer relies on replication recovery from performing
        // the update with `--shardsvr` on.
        var rsName = options.replSet;
        delete options.replSet;
        delete options.shardsvr;
        var mongodConn = MongoRunner.runMongod(options);
        waitForPrimary(mongodConn);

        var res = mongodConn.getDB('admin').system.version.update({_id: 'shardIdentity'},
                                                                  shardIdentityDoc);
        assert.eq(1, res.nModified);

        MongoRunner.stopMongod(mongodConn);

        newMongodOptions.shardsvr = '';
        newMongodOptions.replSet = rsName;
        mongodConn = MongoRunner.runMongod(newMongodOptions);
        waitForPrimary(mongodConn);

        res = mongodConn.getDB('admin').runCommand({shardingState: 1});

        assert(res.enabled);
        assert.eq(shardIdentityDoc.shardName, res.shardName);
        assert.eq(shardIdentityDoc.clusterId, res.clusterId);
        assert.soon(() => shardIdentityDoc.configsvrConnectionString ==
                        mongodConn.adminCommand({shardingState: 1}).configServer);

        return mongodConn;
    };

    // Simulate the upsert that is performed by a config server on addShard.
    assert.commandWorked(mongodConn.getDB('admin').system.version.update(
        {
            _id: shardIdentityDoc._id,
            shardName: shardIdentityDoc.shardName,
            clusterId: shardIdentityDoc.clusterId,
        },
        {$set: {configsvrConnectionString: shardIdentityDoc.configsvrConnectionString}},
        {upsert: true}));

    awaitVersionUpdate();

    var res = mongodConn.getDB('admin').runCommand({shardingState: 1});

    assert(res.enabled);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);
    assert.soon(() => shardIdentityDoc.configsvrConnectionString ==
                    mongodConn.adminCommand({shardingState: 1}).configServer);
    // Should not be allowed to remove the shardIdentity document
    assert.writeErrorWithCode(
        mongodConn.getDB('admin').system.version.remove({_id: 'shardIdentity'}), 40070);

    //
    // Test normal startup
    //

    var newMongodOptions = Object.extend(mongodConn.savedOptions, {
        restart: true,
        // disable snapshotting to force the stable timestamp forward with or without the
        // majority commit point.  This simplifies forcing out our corrupted write to
        // admin.system.version
        setParameter: {"failpoint.disableSnapshotting": "{'mode':'alwaysOn'}"}
    });
    MongoRunner.stopMongod(mongodConn);
    mongodConn = MongoRunner.runMongod(newMongodOptions);
    waitForPrimary(mongodConn);

    res = mongodConn.getDB('admin').runCommand({shardingState: 1});

    assert(res.enabled);
    assert.eq(shardIdentityDoc.shardName, res.shardName);
    assert.eq(shardIdentityDoc.clusterId, res.clusterId);
    assert.soon(() => shardIdentityDoc.configsvrConnectionString ==
                    mongodConn.adminCommand({shardingState: 1}).configServer);

    //
    // Test shardIdentity doc without configsvrConnectionString, resulting into parse error
    //

    // Note: modification of the shardIdentity is allowed only when not running with --shardsvr
    MongoRunner.stopMongod(mongodConn);
    // The manipulation of `--replSet` is explained in `restartAndFixShardIdentityDoc`.
    var rsName = newMongodOptions.replSet;
    delete newMongodOptions.replSet;
    delete newMongodOptions.shardsvr;
    mongodConn = MongoRunner.runMongod(newMongodOptions);
    waitForPrimary(mongodConn);

    let writeResult = assert.commandWorked(mongodConn.getDB('admin').system.version.update(
        {_id: 'shardIdentity'}, {_id: 'shardIdentity', shardName: 'x', clusterId: ObjectId()}));
    assert.eq(writeResult.nModified, 1);

    MongoRunner.stopMongod(mongodConn);

    newMongodOptions.shardsvr = '';
    newMongodOptions.replSet = rsName;
    assert.throws(function() {
        var connToCrashedMongod = MongoRunner.runMongod(newMongodOptions);
        waitForPrimary(connToCrashedMongod);
    });

    // We call MongoRunner.stopMongod() using a former connection to the server that is
    // configured with the same port in order to be able to assert on the server's exit code.
    MongoRunner.stopMongod(mongodConn, undefined, {allowedExitCode: MongoRunner.EXIT_UNCAUGHT});

    //
    // Test that it is possible to fix the invalid shardIdentity doc by not passing --shardsvr
    //
    mongodConn = restartAndFixShardIdentityDoc(newMongodOptions);
    res = mongodConn.getDB('admin').runCommand({shardingState: 1});
    assert(res.enabled);
};

var st = new ShardingTest({shards: 1});

{
    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({shardsvr: ''});
    replTest.initiate();
    runTest(replTest.getPrimary(), st.configRS.getURL(), function() {
        replTest.awaitLastStableRecoveryTimestamp();
    });
    replTest.stopSet();
}

st.stop();
})();
