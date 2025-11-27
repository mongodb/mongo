/**
 * Check the removeShard interoperability with the new remove shard interface.
 * @tags: [
 * requires_fcv_83,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("removeShard interoperability with new removeShard interface", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1, other: {enableBalancer: true}});
        this.rs1 = new ReplSetTest({name: "repl1", nodes: 1});
        this.rs2 = new ReplSetTest({name: "repl2", nodes: 1});
    });

    beforeEach(() => {
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs1.getURL(), name: "repl1"}));
        // Add sharded collections
        assert.commandWorked(this.st.s.adminCommand({enableSharding: "TestDB", primaryShard: "repl1"}));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: "TestDB.Coll", key: {_id: 1}}));
        this.st.s.getDB("TestDB").Coll.insert({_id: -1, value: "Negative value"});
        this.st.s.getDB("TestDB").Coll.insert({_id: 1, value: "Positive value"});

        // Add unsharded collections
        assert.commandWorked(this.st.s.getDB("TestDB").CollUnsharded.insert({_id: 1, value: "Pos"}));

        this.rs2.startSet({shardsvr: ""});
        this.rs2.initiate();
        // Empty shard
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs2.getURL(), name: "repl2"}));
    });

    afterEach(() => {
        // Clean the data
        assert.commandWorked(this.st.s.getDB("TestDB").dropDatabase());

        removeShard(this.st, "repl1");
        removeShard(this.st, "repl2");

        // Stop the replica sets
        this.rs1.stopSet();
        this.rs2.stopSet();

        // Check that the number of shards remains the same after each test case
        assert.eq(1, this.st.s.getDB("config").shards.count());
    });

    after(() => {
        this.st.stop();
    });

    it("startShardDraining interoperability with removeShard", () => {
        // Start draining
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: "repl1"}));

        // Check that removeShard returns 'ongoing'
        const removeStatus_ongoing = this.st.s.adminCommand({removeShard: "repl1"});
        assert.eq("ongoing", removeStatus_ongoing.state);

        // Move the unsharded collection off st.shard1.shardName
        assert.commandWorked(this.st.s.adminCommand({movePrimary: "TestDB", to: this.st.shard0.shardName}));

        // Wait for the shard to be completely drained, then removeShard must remove the shard.
        assert.soon(() => {
            const removeStatus_completed = this.st.s.adminCommand({removeShard: "repl1"});
            return "completed" == removeStatus_completed.state;
        }, "removeShard did not return 'completed' status within the timeout.");
    });

    it("stopShardDraining interoperability with removeShard", () => {
        const config = this.st.s.getDB("config");

        // Start draining the shard with removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: "repl1"}));

        // Stop the draining
        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: "repl1"}));

        // Check that draining has stopped successfully
        const notDrainingShards = config.shards.find({"draining": true}).toArray();

        assert.eq(0, notDrainingShards.length);

        // Check that draining starts again when calling removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: "repl1"}));

        const drainingShards = config.shards.find({"draining": true}).toArray();

        assert.eq(1, drainingShards.length);
        assert.eq("repl1", drainingShards[0]._id);
    });

    it("shardDrainingStatus interoperability with removeShard", () => {
        // Start shard draining with removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: "repl1"}));

        // Check draining status with shardDrainingStatus
        const shardDrainingStatus = this.st.s.adminCommand({shardDrainingStatus: "repl1"});
        assert.commandWorked(shardDrainingStatus);

        // Check draining status with removeShard
        const removeShardStatus = this.st.s.adminCommand({removeShard: "repl1"});
        assert.commandWorked(removeShardStatus);

        assert.eq(shardDrainingStatus.state, removeShardStatus.state);
        assert.eq(shardDrainingStatus.msg, removeShardStatus.msg);
        assert.eq(shardDrainingStatus.dbsToMove, removeShardStatus.dbsToMove);
        assert.eq(shardDrainingStatus.note, removeShardStatus.note);
        assert.eq(shardDrainingStatus.collectionsToMove, removeShardStatus.collectionsToMove);
    });

    it("commitShardRemoval interoperability with removeShard", () => {
        // Start shard draining with removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: "repl2"}));

        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: "repl2"}));

        // Check shard has been removed correctly
        assert.eq(0, this.st.s.getDB("config").shards.find({_id: "repl2"}).toArray().length);
    });
});
