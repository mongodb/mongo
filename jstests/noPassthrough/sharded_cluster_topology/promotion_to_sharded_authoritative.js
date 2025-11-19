/**
 * @tags: [
 *   # This test is incompatible with 'config shard' as it creates a cluster with 0 shards in order
 *   # to be able to add shard with data on it (which is only allowed on the first shard).
 *   config_shard_incompatible,
 *   requires_fcv_83,
 *   # This test restarts the server and requires that data persists across restarts.
 *   requires_persistence,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Promote replica set to shard adds authoritative data", function () {
    before(() => {
        this.st = new ShardingTest({shards: 0, other: {enableBalancer: true}});
        this.rs1 = new ReplSetTest({name: "repl1", nodes: 3});
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();

        // Add data to the replica set
        this.testDB = this.rs1.getPrimary().getDB("TestDB");
        this.testDB.coll.insert({_id: 1, value: "Pos"});
        this.rs1.awaitReplication();

        // Add the replica set as a shard
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs1.getURL()}));
        this.st.configRS.awaitReplication();
        this.rs1.awaitReplication();

        this.getDbMetadataFromGlobalCatalog = function (dbName) {
            return this.st.s.getDB("config").databases.findOne({_id: dbName});
        };

        this.getDbMetadataFromShardCatalog = function (dbName) {
            return this.rs1.getPrimary().getDB("config").shard.catalog.databases.findOne({_id: dbName});
        };

        this.validateShardCatalogCache = function (dbName, shard, expectedDbMetadata) {
            const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
            assert.commandWorked(dbMetadataFromShard);

            if (expectedDbMetadata) {
                assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
            } else {
                assert.eq({}, dbMetadataFromShard.dbVersion);
            }
        };
    });
    after(() => {
        this.rs1.stopSet();
        this.st.stop();
    });

    it("check shard catalog metadata is correct", () => {
        const dbMeta = this.getDbMetadataFromShardCatalog("TestDB");
        const configMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        assert.neq({}, dbMeta);
        assert.eq(dbMeta, configMeta);
    });

    it("check shard catalog cache", () => {
        const expectedMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        this.rs1.nodes.forEach((node) => {
            this.validateShardCatalogCache("TestDB", node, expectedMeta);
        });
    });

    it("check we can read from the database", () => {
        const doc = this.st.s.getDB("TestDB").coll.find({_id: 1}).toArray();
        assert.eq(1, doc.length);
    });

    it("check we can insert documents to the db", () => {
        const db = this.st.s.getDB("TestDB");
        assert.commandWorked(db.coll2.insert({_id: 2, value: "Second doc"}));
    });

    it("check create database works correctly", () => {
        assert.commandWorked(this.st.s.getDB("TestDB2").db.insert({_id: 1, value: "First doc"}));
    });

    it("check that shards have the correct metadata after restart", () => {
        this.st.restartAllShards();
        const dbMeta = this.getDbMetadataFromShardCatalog("TestDB");
        const configMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        assert.neq({}, dbMeta);
        assert.eq(dbMeta, configMeta);
    });
});

describe("Promote replica set to embedded config server adds authoritative data", function () {
    before(() => {
        this.doRollingRestart = (rs, startupFlags) => {
            rs.awaitReplication();
            for (const node of rs.getSecondaries()) {
                const id = rs.getNodeId(node);
                rs.stop(
                    id,
                    null,
                    {},
                    {
                        forRestart: true,
                        waitPid: true,
                    },
                );
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }
            const primaryId = rs.getNodeId(rs.getPrimary());
            const secondary = rs.getSecondary();

            rs.stepUp(secondary, {
                awaitReplicationBeforeStepUp: true,
                awaitWritablePrimary: true,
                doNotWaitForPrimaryOnlyServices: true,
            });

            rs.stop(
                primaryId,
                null,
                {},
                {
                    forRestart: true,
                    waitPid: true,
                },
            );
            assert.doesNotThrow(() => {
                rs.start(primaryId, {
                    ...startupFlags,
                    remember: false,
                });
            });
        };

        this.configRS = new ReplSetTest({nodes: 3});

        this.configRS.startSet({
            replSet: "promotion_to_sharded_authoritative",
            remember: false,
        });
        this.configRS.initiate();

        assert.commandWorked(this.configRS.getPrimary().getDB("TestDB").foo.insertOne({bar: 42}));
        this.configRS.awaitReplication();

        this.doRollingRestart(this.configRS, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });
        this.configRS.awaitReplication();

        let config = {};
        config = this.configRS.getReplSetConfigFromNode();
        config.configsvr = true;
        config.version = config.version + 1;
        assert.commandWorked(this.configRS.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.configRS, {
            configsvr: "",
        });
        this.configRS.awaitReplication();

        this.mongos = MongoRunner.runMongos({configdb: this.configRS.getURL()});

        assert.commandWorked(this.mongos.getDB("admin").runCommand({"transitionFromDedicatedConfigServer": 1}));
        this.configRS.awaitReplication();

        this.getDbMetadataFromGlobalCatalog = function (dbName) {
            return this.mongos.getDB("config").databases.findOne({_id: dbName});
        };

        this.getDbMetadataFromShardCatalog = function (dbName) {
            return this.configRS.getPrimary().getDB("config").shard.catalog.databases.findOne({_id: dbName});
        };

        this.validateShardCatalogCache = function (dbName, shard, expectedDbMetadata) {
            const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
            assert.commandWorked(dbMetadataFromShard);

            if (expectedDbMetadata) {
                assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
            } else {
                assert.eq({}, dbMetadataFromShard.dbVersion);
            }
        };
    });

    after(() => {
        MongoRunner.stopMongos(this.mongos);
        this.configRS.stopSet();
    });

    it("check shard catalog metadata is correct", () => {
        const dbMeta = this.getDbMetadataFromShardCatalog("TestDB");
        const configMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        assert.neq({}, dbMeta);
        assert.eq(dbMeta, configMeta);
    });

    it("check shard catalog cache", () => {
        const expectedMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        this.configRS.nodes.forEach((node) => {
            this.validateShardCatalogCache("TestDB", node, expectedMeta);
        });
    });

    it("check we can read from the database", () => {
        const doc = this.mongos.getDB("TestDB").foo.find({bar: 42}).toArray();
        assert.eq(1, doc.length);
    });

    it("check we can insert documents to the db", () => {
        const db = this.mongos.getDB("TestDB");
        assert.commandWorked(db.coll2.insert({_id: 2, value: "Second doc"}));
    });

    it("check create database works correctly", () => {
        assert.commandWorked(this.mongos.getDB("TestDB2").db.insert({_id: 1, value: "First doc"}));
    });

    it("check that shards have the correct metadata after restart", () => {
        this.doRollingRestart(this.configRS, {
            configsvr: "",
        });
        const dbMeta = this.getDbMetadataFromShardCatalog("TestDB");
        const configMeta = this.getDbMetadataFromGlobalCatalog("TestDB");
        assert.neq({}, dbMeta);
        assert.eq(dbMeta, configMeta);
    });
});
