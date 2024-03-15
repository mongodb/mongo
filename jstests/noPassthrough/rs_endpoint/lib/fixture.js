import {
    getReplicaSetURL,
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

/*
 * Utilities for starting a replica set with --configsvr, promoting it to a sharded cluster, and
 * then scaling the cluster up and down.
 *
 * 1. initiateConfigServerReplicaSet
 *      This initiates a replica set with --configsvr.
 * 2. transitionToOneShardClusterWithConfigShard
 *      This runs transitionFromDedicatedConfigServer against their replica set to turn it to a
 *      config shard. This makes the replica set endpoint become active.
 * 3. transitionToTwoShardClusterWithConfigShard
 *      This runs addShard to add a second shard to the cluster. This makes the replica set
 *      endpoint become inactive.
 * 4. transitionBackToOneShardClusterWithConfigShard
 *      This runs removeShard to remove the second shard to the cluster. This makes the
 *      replica set endpoint become active again.
 * 5. tearDown
 *      This shuts down the sharded cluster.
 */
export var ReplicaSetEndpointTest = class {
    constructor(hasDirectShardOperationPrivilege) {
        jsTest.log("Testing with " + tojson({hasDirectShardOperationPrivilege}));
        this._hasDirectShardOperationPrivilege = hasDirectShardOperationPrivilege;
        this._keyFile = "jstests/libs/key1";
        this._authDbName = "admin";

        this._shard0AdminUser = {
            userName: "admin_shard0",
            password: "admin_shard0_pwd",
            roles: ["root"]
        };
        this._shard0TestUser = {
            userName: "user_shard0",
            password: "user_shard0_pwd",
            roles: ["readWriteAnyDatabase"]
        };
        if (this._hasDirectShardOperationPrivilege) {
            this._shard0TestUser.roles.push("directShardOperations");
        }
        this._shard1AdminUser = {
            userName: "admin_shard1",
            password: "admin_shard1_pwd",
            roles: ["root"]
        };

        // Properties set by initiateConfigServerReplicaSet().
        this._shard0Rst = undefined;
        this._shard0Primary = undefined;
        // This is the client to be used by the caller for testing direct connections to shard0.
        // Some state transitions may require authenticating this client with the admin user to
        // run admin commands but the client is always re-authenticated with the test user
        // (_shard0TestUser) afterwards.
        this.shard0AuthDB = undefined;

        // Properties set by transitionToOneShardClusterWithConfigShard().
        this._mongos = undefined;

        // Properties set by transitionToTwoShardClusterWithConfigShard().
        this._shard1Name = undefined;
        this._shard1Rst = undefined;
        this._shard1Primary = undefined;
        this._shard1AuthDB = undefined;
    }

    _createShard0Users() {
        assert.commandWorked(this.shard0AuthDB.runCommand({
            createUser: this._shard0AdminUser.userName,
            pwd: this._shard0AdminUser.password,
            roles: this._shard0AdminUser.roles
        }));
        assert(this.shard0AuthDB.logout());
        assert(
            this.shard0AuthDB.auth(this._shard0AdminUser.userName, this._shard0AdminUser.password));
        assert.commandWorked(this.shard0AuthDB.runCommand({
            createUser: this._shard0TestUser.userName,
            pwd: this._shard0TestUser.password,
            roles: this._shard0TestUser.roles
        }));
    }

    _createShard1Users() {
        assert.commandWorked(this._shard1AuthDB.runCommand({
            createUser: this._shard1AdminUser.userName,
            pwd: this._shard1AdminUser.password,
            roles: this._shard1AdminUser.roles
        }));
    }

    _authenticateShard0AdminUser() {
        assert(this.shard0AuthDB.logout());
        assert(
            this.shard0AuthDB.auth(this._shard0AdminUser.userName, this._shard0AdminUser.password));
    }

    _authenticateShard0TestUser() {
        assert(this.shard0AuthDB.logout());
        assert(this.shard0AuthDB.auth(this._shard0TestUser.userName, this._shard0TestUser.password))
    }

    _authenticateShard1AdminUser() {
        assert(this._shard1AuthDB.logout());
        assert(this._shard1AuthDB.auth(this._shard1AdminUser.userName,
                                       this._shard1AdminUser.password));
    }

    _waitForHasTwoOrMoreShardsClusterParameter(db, value) {
        assert.soon(() => {
            const parameterRes = assert.commandWorked(
                db.adminCommand({getClusterParameter: "shardedClusterCardinalityForDirectConns"}));
            return parameterRes.clusterParameters[0].hasTwoOrMoreShards == value;
        });
    }

    initiateConfigServerReplicaSet() {
        jsTest.log("Start a replica set with --configsvr. The replica set endpoint is inactive.");
        this._shard0Rst = new ReplSetTest({
            // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage
            // for running db hash check while the replica set is fsync locked.
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    featureFlagReplicaSetEndpoint: true,
                    'failpoint.enforceDirectShardOperationsCheck': "{'mode':'alwaysOn'}"
                }
            },
            isRouterServer: true,
            keyFile: this._keyFile
        });
        this._shard0Rst.startSet({configsvr: '', storageEngine: 'wiredTiger'});
        this._shard0Rst.initiate();
        this._shard0Primary = this._shard0Rst.getPrimary();
        this.shard0AuthDB = this._shard0Primary.getDB(this._authDbName);

        // Create the admin user and local users on shard0.
        this._createShard0Users();
        this._authenticateShard0TestUser();
    }

    transitionToOneShardClusterWithConfigShard() {
        jsTest.log("Promote the replica set to a sharded cluster with config shard. This makes " +
                   "the replica set endpoint become active.");
        this._mongos =
            MongoRunner.runMongos({configdb: this._shard0Rst.getURL(), keyFile: this._keyFile});
        authutil.asCluster(this._mongos, this._keyFile, () => {
            assert.commandWorked(
                this._mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));
        });
    }

    transitionToTwoShardClusterWithConfigShard() {
        this._shard1Name = "shard1";
        this._shard1Rst =
            new ReplSetTest({name: this._shard1Name, nodes: 2, keyFile: this._keyFile});
        this._shard1Rst.startSet({shardsvr: ""});
        this._shard1Rst.initiate();
        this._shard1Primary = this._shard1Rst.getPrimary();
        this._shard1AuthDB = this._shard1Primary.getDB(this._authDbName);

        // Create the admin user on shard1.
        this._createShard1Users();
        this._authenticateShard1AdminUser();
        const shard1URL = getReplicaSetURL(this._shard1AuthDB);

        jsTest.log(
            "Make the sharded cluster have two shards. This makes the replica set endpoint " +
            "become inactive.");
        authutil.asCluster(this._mongos, this._keyFile, () => {
            assert.commandWorked(
                this._mongos.adminCommand({addShard: shard1URL, name: this._shard1Name}));
        });
        this._authenticateShard0AdminUser();
        this._waitForHasTwoOrMoreShardsClusterParameter(this.shard0AuthDB, true);
        this._authenticateShard0TestUser();
    }

    transitionBackToOneShardClusterWithConfigShard() {
        jsTest.log(
            "Make the sharded cluster have only config shard again. This makes the replica " +
            "set endpoint become active again.");
        authutil.asCluster(this._mongos, this._keyFile, () => {
            assert.soon(() => {
                const res = assert.commandWorked(
                    this._mongos.adminCommand({removeShard: this._shard1Name}));
                return res.state == "completed";
            });
        });
    }

    tearDown() {
        MongoRunner.stopMongos(this._mongos);
        this._shard0Rst.stopSet();
        this._shard1Rst.stopSet();
    }
}
