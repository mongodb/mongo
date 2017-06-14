load('jstests/libs/write_concern_util.js');
load('jstests/multiVersion/libs/auth_helpers.js');

/**
 * This file tests that commands that do writes accept a write concern in a sharded cluster. This
 * test defines various database commands and what they expect to be true before and after the fact.
 * It then runs the commands with various invalid writeConcerns and valid writeConcerns and
 * ensures that they succeed and fail appropriately. For the valid writeConcerns, the test stops
 * replication between nodes to make sure the write concern is actually being waited for. This only
 * tests commands that get sent to config servers and must have w: majority specified. If these
 * commands fail, they should return an actual error, not just a writeConcernError.
 */

(function() {
    "use strict";
    var st = new ShardingTest({
        shards: {
            rs0: {nodes: 3, settings: {chainingAllowed: false}},
            rs1: {nodes: 3, settings: {chainingAllowed: false}}
        },
        configReplSetTestOptions: {settings: {chainingAllowed: false}},
        mongos: 1
    });

    var mongos = st.s;
    var dbName = "wc-test-configRS";
    var db = mongos.getDB(dbName);
    var adminDB = mongos.getDB('admin');
    // A database connection on a local shard, rather than through the mongos.
    var localDB = st.shard0.getDB('localWCTest');
    var collName = 'leaves';
    var coll = db[collName];
    var counter = 0;

    function dropTestData() {
        st.configRS.awaitReplication();
        st.rs0.awaitReplication();
        st.rs1.awaitReplication();
        db.dropUser('username');
        db.dropUser('user1');
        localDB.dropUser('user2');
        assert(!db.auth("username", "password"), "auth should have failed");
        getNewDB();
    }

    // We get new databases because we do not want to reuse dropped databases that may be in a
    // bad state. This test calls dropDatabase when config server secondary nodes are down, so the
    // command fails after only the database metadata is dropped from the config servers, but the
    // data on the shards still remains. This makes future operations, such as moveChunk, fail.
    function getNewDB() {
        db = mongos.getDB(dbName + counter);
        counter++;
        coll = db[collName];
    }

    var commands = [];

    commands.push({
        req: {authSchemaUpgrade: 1},
        setupFunc: function() {
            shardCollectionWithChunks(st, coll);
            adminDB.system.version.update(
                {_id: "authSchema"}, {"currentVersion": 3}, {upsert: true});
            localDB.getSiblingDB('admin').system.version.update(
                {_id: "authSchema"}, {"currentVersion": 3}, {upsert: true});

            db.createUser({user: 'user1', pwd: 'pass', roles: jsTest.basicUserRoles});
            assert(db.auth({mechanism: 'MONGODB-CR', user: 'user1', pwd: 'pass'}));
            db.logout();

            localDB.createUser({user: 'user2', pwd: 'pass', roles: jsTest.basicUserRoles});
            assert(localDB.auth({mechanism: 'MONGODB-CR', user: 'user2', pwd: 'pass'}));
            localDB.logout();
        },
        confirmFunc: function() {
            // All users should only have SCRAM credentials.
            verifyUserDoc(db, 'user1', false, true);
            verifyUserDoc(localDB, 'user2', false, true);

            // After authSchemaUpgrade MONGODB-CR no longer works.
            verifyAuth(db, 'user1', 'pass', false, true);
            verifyAuth(localDB, 'user2', 'pass', false, true);
        },
        requiresMajority: true,
        runsOnShards: true,
        failsOnShards: false,
        admin: true
    });

    // Drop an unsharded database.
    commands.push({
        req: {dropDatabase: 1},
        setupFunc: function() {
            coll.insert({type: 'oak'});
            db.pine_needles.insert({type: 'pine'});
        },
        confirmFunc: function() {
            assert.eq(coll.find().itcount(), 0);
            assert.eq(db.pine_needles.find().itcount(), 0);
        },
        requiresMajority: false,
        runsOnShards: true,
        failsOnShards: true,
        admin: false
    });

    // Drop a sharded database.
    commands.push({
        req: {dropDatabase: 1},
        setupFunc: function() {
            shardCollectionWithChunks(st, coll);
            coll.insert({type: 'oak', x: 11});
            db.pine_needles.insert({type: 'pine'});
        },
        confirmFunc: function() {
            assert.eq(coll.find().itcount(), 0);
            assert.eq(db.pine_needles.find().itcount(), 0);
        },
        requiresMajority: false,
        runsOnShards: true,
        failsOnShards: true,
        admin: false
    });

    commands.push({
        req: {createUser: 'username', pwd: 'password', roles: jsTest.basicUserRoles},
        setupFunc: function() {},
        confirmFunc: function() {
            assert(db.auth("username", "password"), "auth failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        failsOnShards: false,
        admin: false
    });

    commands.push({
        req: {updateUser: 'username', pwd: 'password2', roles: jsTest.basicUserRoles},
        setupFunc: function() {
            db.runCommand({createUser: 'username', pwd: 'password', roles: jsTest.basicUserRoles});
        },
        confirmFunc: function() {
            assert(!db.auth("username", "password"), "auth should have failed");
            assert(db.auth("username", "password2"), "auth failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        admin: false
    });

    commands.push({
        req: {dropUser: 'tempUser'},
        setupFunc: function() {
            db.runCommand({createUser: 'tempUser', pwd: 'password', roles: jsTest.basicUserRoles});
            assert(db.auth("tempUser", "password"), "auth failed");
        },
        confirmFunc: function() {
            assert(!db.auth("tempUser", "password"), "auth should have failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        failsOnShards: false,
        admin: false
    });

    // Sharded dropCollection should return a normal error.
    commands.push({
        req: {drop: collName},
        setupFunc: function() {
            shardCollectionWithChunks(st, coll);
        },
        confirmFunc: function() {
            assert.eq(coll.count(), 0);
        },
        requiresMajority: false,
        runsOnShards: true,
        failsOnShards: true,
        admin: false
    });

    // Config server commands require w: majority writeConcerns.
    var invalidWriteConcerns = [{w: 'invalid'}, {w: 2}];

    function testInvalidWriteConcern(wc, cmd) {
        if (wc.w === 2 && !cmd.requiresMajority) {
            return;
        }
        cmd.req.writeConcern = wc;
        jsTest.log("Testing " + tojson(cmd.req));

        dropTestData();
        cmd.setupFunc();
        var res = runCommandCheckAdmin(db, cmd);
        assert.commandFailed(res);
        assert(!res.writeConcernError,
               'bad writeConcern on config server had writeConcernError. ' +
                   tojson(res.writeConcernError));
    }

    function runCommandFailOnShardsPassOnConfigs(cmd) {
        var req = cmd.req;
        var res;
        // This command is run on the shards in addition to the config servers.
        if (cmd.runsOnShards) {
            if (cmd.failsOnShards) {
                // This command fails when there is a writeConcernError on the shards.
                // We set the timeout high enough that the command should not time out against the
                // config server, but not exorbitantly high, because it will always time out against
                // shards and so will increase the runtime of this test.
                req.writeConcern.wtimeout = 15 * 1000;
                res = runCommandCheckAdmin(db, cmd);
                restartReplicationOnAllShards(st);
                assert.commandFailed(res);
                assert(
                    !res.writeConcernError,
                    'command on config servers with a paused replicaset had writeConcernError: ' +
                        tojson(res));
            } else {
                // This command passes and returns a writeConcernError when there is a
                // writeConcernError on the shards.
                // We set the timeout high enough that the command should not time out against the
                // config server, but not exorbitantly high, because it will always time out against
                // shards and so will increase the runtime of this test.
                req.writeConcern.wtimeout = 15 * 1000;
                res = runCommandCheckAdmin(db, cmd);
                restartReplicationOnAllShards(st);
                assert.commandWorked(res);
                cmd.confirmFunc();
                assertWriteConcernError(res);
            }
        } else {
            // This command is only run on the config servers and so should pass when shards are
            // not replicating.
            res = runCommandCheckAdmin(db, cmd);
            restartReplicationOnAllShards(st);
            assert.commandWorked(res);
            cmd.confirmFunc();
            assert(!res.writeConcernError,
                   'command on config servers with a paused replicaset had writeConcernError: ' +
                       tojson(res));
        }
    }

    function testMajorityWriteConcern(cmd) {
        var req = cmd.req;
        var setupFunc = cmd.setupFunc;
        var confirmFunc = cmd.confirmFunc;

        req.writeConcern = {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS};
        jsTest.log("Testing " + tojson(req));

        dropTestData();
        setupFunc();

        // Command with a full cluster should succeed.
        var res = runCommandCheckAdmin(db, cmd);
        assert.commandWorked(res);
        assert(!res.writeConcernError,
               'command on a full cluster had writeConcernError: ' + tojson(res));
        confirmFunc();

        dropTestData();
        setupFunc();
        // Stop replication at all shard secondaries.
        stopReplicationOnSecondariesOfAllShards(st);

        // Command is running on full config server replica set but a majority of a shard's
        // nodes are down.
        runCommandFailOnShardsPassOnConfigs(cmd);

        dropTestData();
        setupFunc();
        // Stop replication at all config server secondaries and all shard secondaries.
        stopReplicationOnSecondariesOfAllShards(st);
        st.configRS.awaitReplication();
        stopReplicationOnSecondaries(st.configRS);

        // Command should fail after two config servers are not replicating.
        req.writeConcern.wtimeout = 3000;
        res = runCommandCheckAdmin(db, cmd);
        restartReplicationOnAllShards(st);
        assert.commandFailed(res);
        assert(!res.writeConcernError,
               'command on config servers with a paused replicaset had writeConcernError: ' +
                   tojson(res));
    }

    commands.forEach(function(cmd) {
        invalidWriteConcerns.forEach(function(wc) {
            testInvalidWriteConcern(wc, cmd);
        });
        testMajorityWriteConcern(cmd);
    });

})();
