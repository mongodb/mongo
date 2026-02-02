/**
 * Tests that add shard fails with replicasets that have any sharding related data
 *
 * @tags: [
 *   config_shard_incompatible,
 *   # This restriction was introduced in binary v8.3.
 *   requires_fcv_83,
 *   # we restart the replicaset and expect to hold the state
 *   requires_persistence,
 * ]
 */

import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("add shard with refurbished replicaset", function () {
    before(() => {
        this.removeShardIdentity = (rs) => {
            const serverConfigurationCollection = rs.getPrimary().getDB("admin")["system.version"];
            assert.commandWorked(serverConfigurationCollection.deleteOne({_id: "shardIdentity"}));
        };

        this.dropDatabase = (rs, db) => {
            assert.commandWorked(rs.getPrimary().getDB(db).dropDatabase());
        };

        this.dropMetadata = (rs) => {
            const config = rs.getPrimary().getDB("config");
            assert(config.getCollection("shard.collections").drop());
            assert(config.getCollection("shard.catalog.databases").drop());
            assert(config.getCollection("shard.catalog.collections").drop());
            assert(config.getCollection("shard.catalog.chunks").drop());
            assert(config.getCollection("vectorClock").drop());
            assert(config.getCollection("databases").drop());
            assert(config.getCollection("chunks").drop());
            assert(config.getCollection("collections").drop());
            assert(config.getCollection("placementHistory").drop());
            assert(config.getCollection("tags").drop());
            assert(config.getCollection("version").drop());
            assert(config.getCollection("mongos").drop());
            assert(config.getCollection("shards").drop());
        };

        this.restartAsShardsvr = (rs) => {
            rs.stopSet(null, true, {remember: false, skipValidation: true});
            rs.startSet({shardsvr: "", remember: false}, true);
            rs.waitForPrimary();
        };
    });

    beforeEach(() => {
        this.st = new ShardingTest({name: "former-cluster", shards: 0, mongos: 1, config: 1, rs: {nodes: 1}});

        this.rs = new ReplSetTest({name: "data", nodes: 1});
        this.rs.startSet({replSet: "data", shardsvr: "", remember: false});
        this.rs.initiate();
        this.rs.waitForPrimary();

        this.shardName = "newShard";

        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}));

        this.dbName = "test";
        this.collectionName = "foo";
        assert.commandWorked(this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.shardName}));

        this.collection = this.st.s.getDB(this.dbName)[this.collectionName];

        assert.commandWorked(this.collection.insertOne({value: "foo"}));

        this.rs.stopSet(null, true, {remember: false, skipValidation: true});

        this.st.stop({skipValidation: true});
        this.st = new ShardingTest({name: "new-cluster", shards: 0, mongos: 1, config: 1, rs: {nodes: 1}});

        this.rs.startSet({replSet: "data", remember: false}, true);
        this.rs.waitForPrimary();
    });

    afterEach(() => {
        this.rs.stopSet();
        this.st.stop({skipValidation: true});
    });

    it("cleanup: shardIdentity", () => {
        this.removeShardIdentity(this.rs);
        this.restartAsShardsvr(this.rs);
        assert.commandFailedWithCode(
            this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("cleanup: shardIdentity, data", () => {
        this.removeShardIdentity(this.rs);
        this.dropDatabase(this.rs, this.dbName);
        this.restartAsShardsvr(this.rs);
        assert.commandFailedWithCode(
            this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("cleanup: shardIdentity, data, metadata", () => {
        this.removeShardIdentity(this.rs);
        this.dropDatabase(this.rs, this.dbName);
        this.dropMetadata(this.rs);
        this.restartAsShardsvr(this.rs);
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}));
    });

    it("cleanup: shardIdentity, metadata", () => {
        this.removeShardIdentity(this.rs);
        this.dropMetadata(this.rs);
        this.restartAsShardsvr(this.rs);
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}));
    });
});
