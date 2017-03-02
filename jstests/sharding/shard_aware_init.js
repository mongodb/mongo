/**
 * Tests for shard aware initialization during process startup (for standalone) and transition
 * to primary (for replica set nodes).
 * Note: test will deliberately cause a bongod instance to terminate abruptly and bongod instance
 * without journaling will complain about unclean shutdown.
 * @tags: [requires_persistence, requires_journaling]
 */

(function() {
    "use strict";

    var waitForMaster = function(conn) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({isMaster: 1});
            return res.ismaster;
        });
    };

    /**
     * Runs a series of test on the bongod instance bongodConn is pointing to. Notes that the
     * test can restart the bongod instance several times so bongodConn can end up with a broken
     * connection after.
     */
    var runTest = function(bongodConn, configConnStr) {
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
            delete options.shardsvr;
            bongodConn = BongoRunner.runBongod(options);
            waitForMaster(bongodConn);

            var res = bongodConn.getDB('admin').system.version.update({_id: 'shardIdentity'},
                                                                      shardIdentityDoc);
            assert.eq(1, res.nModified);

            BongoRunner.stopBongod(bongodConn.port);

            newBongodOptions.shardsvr = '';
            bongodConn = BongoRunner.runBongod(newBongodOptions);
            waitForMaster(bongodConn);

            res = bongodConn.getDB('admin').runCommand({shardingState: 1});

            assert(res.enabled);
            assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
            assert.eq(shardIdentityDoc.shardName, res.shardName);
            assert.eq(shardIdentityDoc.clusterId, res.clusterId);

            return bongodConn;
        };

        // Simulate the upsert that is performed by a config server on addShard.
        var shardIdentityQuery = {
            _id: shardIdentityDoc._id,
            shardName: shardIdentityDoc.shardName,
            clusterId: shardIdentityDoc.clusterId,
        };
        var shardIdentityUpdate = {
            $set: {configsvrConnectionString: shardIdentityDoc.configsvrConnectionString}
        };
        assert.writeOK(bongodConn.getDB('admin').system.version.update(
            shardIdentityQuery, shardIdentityUpdate, {upsert: true}));

        var res = bongodConn.getDB('admin').runCommand({shardingState: 1});

        assert(res.enabled);
        assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
        assert.eq(shardIdentityDoc.shardName, res.shardName);
        assert.eq(shardIdentityDoc.clusterId, res.clusterId);
        // Should not be allowed to remove the shardIdentity document
        assert.writeErrorWithCode(
            bongodConn.getDB('admin').system.version.remove({_id: 'shardIdentity'}), 40070);

        //
        // Test normal startup
        //

        var newBongodOptions = Object.extend(bongodConn.savedOptions, {restart: true});

        BongoRunner.stopBongod(bongodConn.port);
        bongodConn = BongoRunner.runBongod(newBongodOptions);
        waitForMaster(bongodConn);

        res = bongodConn.getDB('admin').runCommand({shardingState: 1});

        assert(res.enabled);
        assert.eq(shardIdentityDoc.configsvrConnectionString, res.configServer);
        assert.eq(shardIdentityDoc.shardName, res.shardName);
        assert.eq(shardIdentityDoc.clusterId, res.clusterId);

        //
        // Test shardIdentity doc without configsvrConnectionString, resulting into parse error
        //

        // Note: modification of the shardIdentity is allowed only when not running with --shardsvr
        BongoRunner.stopBongod(bongodConn.port);
        delete newBongodOptions.shardsvr;
        bongodConn = BongoRunner.runBongod(newBongodOptions);
        waitForMaster(bongodConn);

        assert.writeOK(bongodConn.getDB('admin').system.version.update(
            {_id: 'shardIdentity'}, {_id: 'shardIdentity', shardName: 'x', clusterId: ObjectId()}));

        BongoRunner.stopBongod(bongodConn.port);

        newBongodOptions.shardsvr = '';
        assert.throws(function() {
            bongodConn = BongoRunner.runBongod(newBongodOptions);
            waitForMaster(bongodConn);
        });

        //
        // Test that it is possible to fix the invalid shardIdentity doc by not passing --shardsvr
        //

        try {
            // The server was terminated not by calling stopBongod earlier, this will cleanup
            // the process from registry in shell_utils_launcher.
            BongoRunner.stopBongod(newBongodOptions.port);
        } catch (ex) {
            if (!(ex instanceof (BongoRunner.StopError))) {
                throw ex;
            }
        }

        bongodConn = restartAndFixShardIdentityDoc(newBongodOptions);
        res = bongodConn.getDB('admin').runCommand({shardingState: 1});
        assert(res.enabled);
    };

    var st = new ShardingTest({shards: 1});

    var bongod = BongoRunner.runBongod({shardsvr: ''});

    runTest(bongod, st.configRS.getURL());

    BongoRunner.stopBongod(bongod.port);

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({shardsvr: ''});
    replTest.initiate();

    runTest(replTest.getPrimary(), st.configRS.getURL());

    replTest.stopSet();

    st.stop();

})();
