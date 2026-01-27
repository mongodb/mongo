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
            assert(rs.getPrimary().getDB("config")["shard.collections"].drop());
            assert(rs.getPrimary().getDB("config")["shard.catalog.databases"].drop());
            assert(rs.getPrimary().getDB("config")["shard.catalog.collections"].drop());
            assert(rs.getPrimary().getDB("config")["shard.catalog.chunks"].drop());
        };

        this.dropVectorClock = (rs) => {
            assert(rs.getPrimary().getDB("config")["vectorClock"].drop());
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
        assert.commandFailedWithCode(
            this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("cleanup: shardIdentity, data, metadata, vectorClock", () => {
        this.removeShardIdentity(this.rs);
        this.dropDatabase(this.rs, this.dbName);
        this.dropMetadata(this.rs);
        this.dropVectorClock(this.rs);
        this.restartAsShardsvr(this.rs);
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs.getURL(), name: this.shardName}));
    });
});
