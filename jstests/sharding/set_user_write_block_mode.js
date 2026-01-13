/**
 * Tests sharding specific functionality of the setUserWriteBlockMode command. Non sharding specific
 * aspects of this command should be checked on
 * jstests/noPassthrough/security/set_user_write_block_mode.js instead.
 *
 * @tags: [
 *   requires_fcv_60,
 *   # The test caches authenticated connections, so we do not support stepdowns
 *   does_not_support_stepdowns,
 *   # The test creates a sharded cluster with a dedicated config server, so the test is incompatible with fixtures with embedded config servers
 *   config_shard_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe.only("setUserWriteBlockMode on replicaset", function () {
    before(() => {
        this.is_83 = () => {
            const admin = this.rs.getPrimary().getDB("admin");
            const res = admin.system.version.find({_id: "featureCompatibilityVersion"}).toArray();
            const is_83 = res.length == 0 || MongoRunner.compareBinVersions(res[0].version, "8.3") >= 0;
            return is_83;
        };

        this.test = () => {
            const admin = this.rs.getPrimary().getDB("admin");

            const newShardDB = "newShardDB";
            const newShardColl = "newShardColl";
            assert.commandWorked(this.rs.getPrimary().getDB(newShardDB).getCollection(newShardColl).insert({x: 1}));

            assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: true}));

            assert.commandFailedWithCode(
                this.rs.getPrimary().getDB(newShardDB).getCollection(newShardColl).insert({x: 2}),
                ErrorCodes.UserWritesBlocked,
            );

            assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: false}));

            assert.commandWorked(this.rs.getPrimary().getDB(newShardDB).getCollection(newShardColl).insert({x: 2}));
        };
    });

    beforeEach(() => {
        this.rs = new ReplSetTest({nodes: 1});
        this.rs.startSet();
        this.rs.initiate();
    });

    afterEach(() => {
        this.rs.stopSet();
    });

    it("user write blocking works on replicaset", () => {
        this.test();
    });

    it("user write blocking works on shard rs", () => {
        if (!this.is_83()) {
            return;
        }

        this.rs.stopSet(null, /* forRestart: */ true, {});
        this.rs.startSet({shardsvr: ""}, true);

        this.test();
    });

    it("user write blocking works on rs with replicaSetConfigShardMaintenanceMode", () => {
        if (!this.is_83()) {
            return;
        }

        this.rs.stopSet(null, /* forRestart: */ true, {});
        this.rs.startSet({replicaSetConfigShardMaintenanceMode: ""}, true);

        this.test();
    });
});

describe("setUserWriteBlockMode during addShard and removeShard", function () {
    before(() => {
        this._shards = [];

        this.createShard = (shardName) => {
            const newShard = new ReplSetTest({name: shardName, nodes: 1});
            newShard.startSet({shardsvr: ""});
            newShard.initiate();
            this._shards.push({name: shardName, shard: newShard});
            return newShard;
        };
    });

    beforeEach(() => {
        this.cluster = new ShardingTest({shards: 0});
    });

    afterEach(() => {
        this.cluster.stop();
        this._shards.forEach((shard) => {
            shard.shard.stopSet();
        });
        this._shards = [];
    });

    it("addShard sets the proper user writes blocking state on the new shard", () => {
        const newShardName = "newShard";
        const newShard = this.createShard(newShardName);

        const newShardDB = "newShardDB";
        const newShardColl = "newShardColl";
        assert.commandWorked(newShard.getPrimary().getDB(newShardDB).getCollection(newShardColl).insert({x: 1}));

        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: true}));
        assert.commandWorked(this.cluster.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));

        assert.commandFailedWithCode(
            this.cluster.s.getDB(newShardDB).getCollection(newShardColl).insert({x: 2}),
            ErrorCodes.UserWritesBlocked,
        );
        assert.commandFailedWithCode(
            newShard.getPrimary().getDB(newShardDB).getCollection(newShardColl).insert({x: 2}),
            ErrorCodes.UserWritesBlocked,
        );
    });
});

describe("setUserWriteBlockMode on cluster", function () {
    before(() => {
        this.cluster = new ShardingTest({shards: 2});
    });

    beforeEach(() => {
        this.dbName = "test";
        this.collName = "foo";
        this.ns = this.dbName + "." + this.collName;

        assert.commandWorked(
            this.cluster.s.adminCommand({enableSharding: this.dbName, primaryShard: this.cluster.shard0.shardName}),
        );

        this.db = this.cluster.s.getDB(this.dbName);
        this.coll = this.db[this.collName];
    });

    afterEach(() => {
        this.cluster.s.getDB(this.dbName).dropDatabase();
    });

    after(() => {
        this.cluster.stop();
    });

    it("addShard serializes with setUserWriteBlockMode", () => {
        // Start setUserWriteBlockMode and make it hang during the SetUserWriteBlockModeCoordinator execution.
        let hangInShardsvrSetUserWriteBlockModeFailPoint = configureFailPoint(
            this.cluster.shard0,
            "hangInShardsvrSetUserWriteBlockMode",
        );
        let awaitShell = startParallelShell(() => {
            assert.commandWorked(db.adminCommand({setUserWriteBlockMode: 1, global: true}));
        }, this.cluster.s.port);
        hangInShardsvrSetUserWriteBlockModeFailPoint.wait();

        const newShardName = "newShard";
        const newShard = new ReplSetTest({name: newShardName, nodes: 1});
        newShard.startSet({shardsvr: ""});
        newShard.initiate();

        assert.commandFailedWithCode(
            this.cluster.s.adminCommand({addShard: newShard.getURL(), name: newShardName, maxTimeMS: 1000}),
            ErrorCodes.MaxTimeMSExpired,
        );

        hangInShardsvrSetUserWriteBlockModeFailPoint.off();
        awaitShell();

        assert.commandWorked(this.cluster.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
        removeShard(this.cluster, newShardName);

        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: false}));

        newShard.stopSet();
    });

    it("chunk migrations work even if user writes are blocked", () => {
        assert.commandWorked(this.cluster.s.adminCommand({shardCollection: this.ns, key: {_id: 1}}));
        // Insert a document to the chunk that will be migrated to ensure that the recipient will at
        // least insert one document as part of the migration.
        this.coll.insert({_id: 1});

        // Create an index to check that the recipient, upon receiving its first chunk, can create it.
        const indexKey = {x: 1};
        assert.commandWorked(this.coll.createIndex(indexKey));

        // Start blocking user writes.
        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

        // Move one chunk to shard1.
        assert.commandWorked(this.cluster.s.adminCommand({split: this.ns, middle: {_id: 0}}));
        assert.commandWorked(
            this.cluster.s.adminCommand({moveChunk: this.ns, find: {_id: 0}, to: this.cluster.shard1.shardName}),
        );

        assert.eq(1, this.coll.find({_id: 1}).itcount());
        assert.eq(1, this.cluster.shard1.getDB(this.dbName)[this.collName].find({_id: 1}).itcount());

        // Check that the index has been created on recipient.
        ShardedIndexUtil.assertIndexExistsOnShard(this.cluster.shard1, this.dbName, this.collName, indexKey);

        // Check that orphans are deleted from the donor.
        assert.soon(() => {
            return this.cluster.shard0.getDB(this.dbName)[this.collName].find({_id: 1}).itcount() === 0;
        });

        // Create an extra index on shard1. This index is not present in the shard0, thus shard1 will
        // drop it when it receives its first chunk.
        {
            // Leave shard1 without any chunk.
            assert.commandWorked(
                this.cluster.s.adminCommand({moveChunk: this.ns, find: {_id: 0}, to: this.cluster.shard0.shardName}),
            );

            // Create an extra index on shard1.
            assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
            const extraIndexKey = {y: 1};
            assert.commandWorked(this.cluster.shard1.getDB(this.dbName)[this.collName].createIndex(extraIndexKey));
            assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

            // Move a chunk to shard1.
            assert.commandWorked(
                this.cluster.s.adminCommand({moveChunk: this.ns, find: {_id: 0}, to: this.cluster.shard1.shardName}),
            );

            // Check the mismatched index was dropped.
            ShardedIndexUtil.assertIndexDoesNotExistOnShard(
                this.cluster.shard1,
                this.dbName,
                this.collName,
                extraIndexKey,
            );
        }

        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
    });

    it("movePrimary works while user writes are blocked", () => {
        // Create an unsharded collection so that its data needs to be cloned to the new db-primary.
        const unshardedCollName = "unshardedColl";
        const unshardedColl = this.db[unshardedCollName];
        assert.commandWorked(unshardedColl.createIndex({x: 1}));
        unshardedColl.insert({x: 1});

        // Start blocking user writes
        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

        const fromShard = this.cluster.getPrimaryShard(this.dbName);
        const toShard = this.cluster.getOther(fromShard);
        assert.commandWorked(this.cluster.s.adminCommand({movePrimary: this.dbName, to: toShard.name}));

        // Check that the new primary has cloned the data.
        assert.eq(1, toShard.getDB(this.dbName)[unshardedCollName].find().itcount());

        // Check that the collection has been removed from the former primary.
        assert.eq(
            0,
            fromShard.getDB(this.dbName).runCommand({listCollections: 1, filter: {name: unshardedCollName}}).cursor
                .firstBatch.length,
        );

        // Check that the database primary has been changed to the new primary.
        assert.eq(
            1,
            this.cluster.s
                .getDB("config")
                .getCollection("databases")
                .find({_id: this.dbName, primary: toShard.shardName})
                .itcount(),
        );

        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
    });

    it("setAllowMigrations works while user writes are blocked", () => {
        assert.commandWorked(this.cluster.s.adminCommand({shardCollection: this.ns, key: {_id: 1}}));

        // Start blocking user writes.
        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

        // Disable migrations for 'ns'.
        assert.commandWorked(this.cluster.s.adminCommand({setAllowMigrations: this.ns, allowMigrations: false}));

        const fromShard = this.cluster.getPrimaryShard(this.dbName);
        const toShard = this.cluster.getOther(fromShard);
        assert.commandFailedWithCode(
            this.cluster.s.adminCommand({moveChunk: this.ns, find: {_id: 0}, to: toShard.shardName}),
            ErrorCodes.ConflictingOperationInProgress,
        );

        // Reenable migrations for 'ns'.
        assert.commandWorked(this.cluster.s.adminCommand({setAllowMigrations: this.ns, allowMigrations: true}));

        assert.commandWorked(this.cluster.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
    });
});

describe("setUserWriteBlockMode on cluster through direct connection", function () {
    before(function () {
        this.keyFile = "jstests/libs/key1";

        this.createAdminUser = function (conn) {
            const directConnection = conn.getDB("admin");
            directConnection.createUser({user: "admin", pwd: "x", roles: ["clusterManager", "restore"]});
            assert(directConnection.auth("admin", "x"), "Authentication failed");
        };
    });

    beforeEach(function () {
        this.cluster = new ShardingTest({name: "st", shards: 0, other: {keyFile: this.keyFile}});
        this.createAdminUser(this.cluster);

        this.rs = new ReplSetTest({name: "rs", nodes: 1, keyFile: this.keyFile});
        this.rs.startSet({shardsvr: ""});
        this.rs.initiate();
        this.createAdminUser(this.rs.getPrimary());

        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.rs.getURL(), name: "newShards"}));
    });

    afterEach(function () {
        this.cluster.stop();
        this.rs.stopSet();
    });

    it("setUserWriteBlockMode is prohibited", () => {
        assert(this.rs.getPrimary().getDB("admin").auth("admin", "x"), "Authentication failed");

        const admin = this.rs.getPrimary().getDB("admin");
        const res = admin.system.version.find({_id: "featureCompatibilityVersion"}).toArray();
        const is_83 = res.length == 0 || MongoRunner.compareBinVersions(res[0].version, "8.3") >= 0;
        if (!is_83) {
            return;
        }

        assert.commandFailedWithCode(
            this.rs.getPrimary().getDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}),
            ErrorCodes.Unauthorized,
        );
    });
});
