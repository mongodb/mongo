/**
 * Testing of promoting and demoting a replicaset to sharded cluster with embedded csrs
 * movement
 * @tags: [
 *   requires_persistence,
 *   requires_fcv_83,
 *   # The test caches authenticated connections, so we do not support stepdowns
 *   does_not_support_stepdowns,
 * ]
 */

import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("promote and demote replicaset to sharded cluster", function() {
    before(() => {
        this.keyFile = "jstests/libs/key1";

        this.doRollingRestart = (rs, startupFlags) => {
            rs.awaitReplication();
            for (const node of rs.getSecondaries()) {
                const id = rs.getNodeId(node);
                rs.stop(id, null, {}, {
                    forRestart: true,
                    waitPid: true,
                });
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }

            const primaryId = rs.getNodeId(rs.getPrimary());
            const secondary = rs.getSecondary();

            // We wait for the stepup writes explicitly as the stepup calls the waitForStepUpWrites
            // on the primary, but we call the asCluster on the secondary (so it will hang there,
            // because of lack of authorization).
            rs.asCluster(rs.getPrimary(), () => {
                rs.waitForStepUpWrites();
            }, this.keyFile);

            rs.asCluster(secondary, () => {
                rs.stepUp(secondary, {
                    awaitReplicationBeforeStepUp: true,
                    awaitWritablePrimary: true,
                    doNotWaitForPrimaryOnlyServices: true,
                });
            }, this.keyFile);

            rs.stop(primaryId, null, {}, {
                forRestart: true,
                waitPid: true,
            });
            assert.doesNotThrow(() => {
                rs.start(primaryId, {
                    ...startupFlags,
                    remember: false,
                });
            });
        };

        this.shards = [];

        // TODO(SERVER-100403): remove ShardAuthoritative parameters
        const shardParameters = {
            featureFlagShardAuthoritativeDbMetadataDDL: false,
            featureFlagShardAuthoritativeDbMetadataCRUD: false,
            featureFlagShardAuthoritativeCollMetadata: false,
        };

        this.addNewShard = () => {
            const name = `shard-${this.shards.length}`;
            const rs = new ReplSetTest({
                name,
                nodes: [
                    {setParameter: shardParameters},
                    {setParameter: shardParameters},
                    {setParameter: shardParameters},
                ],
                keyFile: this.keyFile,
            });

            rs.startSet({shardsvr: ""});
            rs.initiate();

            assert.commandWorked(
                this.mongos.getDB("admin").runCommand({"addShard": rs.getURL(), name}));

            this.shards.push(rs);
        };

        this.configRS = new ReplSetTest({
            nodes: [
                {setParameter: shardParameters},
                {setParameter: shardParameters},
                {setParameter: shardParameters},
            ],
            keyFile: this.keyFile,
        });
    });

    beforeEach(() => {
        this.configRS.startSet({
            replSet: "replica_set_promotion_demotion",
            remember: false,
        });

        this.configRS.asCluster(this.configRS.nodes[0], () => {
            this.configRS.initiate();
        }, this.keyFile);

        const adminDBDirectConnection = this.configRS.getPrimary().getDB("admin");
        adminDBDirectConnection.createUser({user: "admin", pwd: 'x', roles: ["__system"]});
        assert(adminDBDirectConnection.auth("admin", 'x'), "Authentication failed");
        this.configRS.getPrimary().getDB("test").createUser(
            {user: "user", pwd: "x", roles: ["readWrite"]});
        this.userDirectConnection = new Mongo(this.configRS.getPrimary().host);
        this.testDBDirectConnection = this.userDirectConnection.getDB("test");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");

        assert.commandWorked(this.testDBDirectConnection.foo.insertOne({bar: 42}));

        this.doRollingRestart(this.configRS, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            const res = assert.commandWorked(
                this.configRS.getPrimary().getDB('admin').runCommand({shardingState: 1}));
            assert.eq(res.enabled, false);
        });

        let config = {};
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            config = this.configRS.getReplSetConfigFromNode();
        }, this.keyFile);
        config.configsvr = true;
        config.version = config.version + 1;
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            assert.commandWorked(
                this.configRS.getPrimary().adminCommand({replSetReconfig: config}));
        }, this.keyFile);
        this.doRollingRestart(this.configRS, {
            configsvr: "",
        });

        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            const res = assert.commandWorked(
                this.configRS.getPrimary().getDB('admin').runCommand({shardingState: 1}));
            assert.eq(res.enabled, false);
        });

        this.mongos =
            MongoRunner.runMongos({keyFile: this.keyFile, configdb: this.configRS.getURL()});

        this.adminDBMongosConnection = this.mongos.getDB("admin");
        assert(this.adminDBMongosConnection.auth("admin", 'x'), "Authentication failed");
        assert.commandWorked(
            this.adminDBMongosConnection.runCommand({"transitionFromDedicatedConfigServer": 1}));

        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            const res = assert.commandWorked(
                this.configRS.getPrimary().getDB('admin').runCommand({shardingState: 1}));
            assert.eq(res.enabled, true);
        });

        // Resetting connections as during the rolling restart we lost them
        this.userDirectConnection = new Mongo(this.configRS.getPrimary().host);
        this.testDBDirectConnection = this.userDirectConnection.getDB("test");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");
    });

    afterEach(() => {
        MongoRunner.stopMongos(this.mongos);
        for (const rs of this.shards) {
            rs.stopSet();
        }
        this.shards = [];
        this.configRS.stopSet();
    });

    it("promotion to cluster keeps data", () => {
        assert.eq(this.testDBDirectConnection.foo.count({bar: 42}), 1);
        assert.eq(this.mongos.getDB("test").foo.count({bar: 42}), 1);
    });

    it("direct operation is allowed after promotion", () => {
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            assert.commandWorked(this.configRS.getPrimary().getDB("admin").runCommand(
                {_flushRoutingTableCacheUpdates: "test.foo"}));
        }, this.keyFile);
        assert.commandWorked(this.testDBDirectConnection.foo.insertOne({baz: -1}));
        assert.eq(this.testDBDirectConnection.foo.count({baz: -1}), 1);
        assert.eq(this.mongos.getDB("test").foo.count({baz: -1}), 1);
    });

    it("adding new shard prohibits direct connections", () => {
        this.addNewShard();
        assert.eq(this.mongos.getDB("test").foo.count({bar: 42}), 1);
        assert.commandFailedWithCode(
            this.testDBDirectConnection.runCommand({count: "foo", query: {bar: 42}}),
            ErrorCodes.Unauthorized);
    });

    it("moving to dedicated", () => {
        this.addNewShard();
        assert.commandWorked(
            this.adminDBMongosConnection.runCommand({"transitionToDedicatedConfigServer": 1}));
        assert.commandWorked(
            this.adminDBMongosConnection.runCommand({movePrimary: "test", to: "shard-0"}));
        assert.soon(() => {
            const res = assert.commandWorked(
                this.adminDBMongosConnection.runCommand({"transitionToDedicatedConfigServer": 1}));
            return res.status !== "completed";
        });
        assert.eq(this.mongos.getDB("test").foo.count({bar: 42}), 1);
    });

    it("demoting keeps data", () => {
        assert.commandWorked(this.mongos.getDB("test").foo.insertOne({baz: -1}));

        this.doRollingRestart(this.configRS, {
            replSet: "replica_set_promotion_demotion",
            replicaSetConfigShardMaintenanceMode: "",
        });

        let config = {};
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            config = this.configRS.getReplSetConfigFromNode();
        }, this.keyFile);
        delete config.configsvr;
        config.version = config.version + 1;
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            assert.commandWorked(
                this.configRS.getPrimary().adminCommand({replSetReconfig: config}));
        }, this.keyFile);

        this.doRollingRestart(this.configRS, {
            replSet: "replica_set_promotion_demotion",
        });

        const conn = new Mongo(this.configRS.getPrimary().host);
        const testDB = conn.getDB("test");
        assert(testDB.auth("user", "x"), "Authentication failed");

        assert.eq(testDB.foo.count({bar: 42}), 1);
        assert.eq(testDB.foo.count({baz: -1}), 1);
    });
});
