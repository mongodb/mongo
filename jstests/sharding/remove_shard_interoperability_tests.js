/**
 * Check the startShardDraining and removeShard interoperability
 * @tags: [
 * requires_fcv_82,
 * # TODO(SERVER-108416): Remove exclusion when 8.2 -> 8.3 FCV change finishes
 * multiversion_incompatible,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("removeShard interoperability with new removeShard interface", function() {
    before(() => {
        this.st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
        this.rs1 = new ReplSetTest({nodes: 1});
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();
        assert.commandWorked(
            this.st.s.adminCommand({addShard: this.rs1.getURL(), name: this.rs1.name}));
    });

    beforeEach(() => {
        // Add sharded collections
        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: 'TestDB', primaryShard: this.rs1.name}));
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: 'TestDB.Coll', key: {_id: 1}}));
        this.st.s.getDB('TestDB').Coll.insert({_id: -1, value: 'Negative value'});
        this.st.s.getDB('TestDB').Coll.insert({_id: 1, value: 'Positive value'});

        // Add unsharded collections
        assert.commandWorked(
            this.st.s.getDB("TestDB").CollUnsharded.insert({_id: 1, value: "Pos"}));
    });

    afterEach(() => {
        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: this.rs1.name}));

        // Clean the data
        assert.commandWorked(this.st.s.getDB('TestDB').dropDatabase());

        // Check that the number of shards remains the same after each test case
        assert.eq(3, this.st.s.getDB("config").shards.count());
    });

    after(() => {
        this.rs1.stopSet();
        this.st.stop();
    });

    it("startShardDraining interoperability with removeShard", () => {
        // Start draining
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: this.rs1.name}));

        // Check that removeShard returns 'ongoing'
        const removeStatus_ongoing = this.st.s.adminCommand({removeShard: this.rs1.name});
        assert.eq('ongoing', removeStatus_ongoing.state);

        // Move the unsharded collection off st.shard1.shardName
        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: "TestDB", to: this.st.shard0.shardName}));

        // Wait for the shard to be completely drained, then removeShard must remove the shard.
        assert.soon(() => {
            const removeStatus_completed = this.st.s.adminCommand({removeShard: this.rs1.name});
            return 'completed' == removeStatus_completed.state;
        }, "removeShard did not return 'completed' status within the timeout.");

        // Restart the replica set and add it back to the cluster
        this.rs1.stopSet();
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();
        assert.commandWorked(
            this.st.s.adminCommand({addShard: this.rs1.getURL(), name: this.rs1.name}));
    });

    it("stopShardDraining interoperability with removeShard", () => {
        const config = this.st.s.getDB('config');

        // Start draining the shard with removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: this.st.shard1.shardName}));

        // Stop the draining
        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: this.st.shard1.shardName}));

        // Check that draining has stopped successfully
        const notDrainingShards = config.shards.find({'draining': true}).toArray();

        assert.eq(0, notDrainingShards.length);

        // Check that draining starts again when calling removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: this.st.shard1.shardName}));

        const drainingShards = config.shards.find({'draining': true}).toArray();

        assert.eq(1, drainingShards.length);
        assert.eq(this.st.shard1.shardName, drainingShards[0]._id);
    });

    it("shardDrainingStatus interoperability with removeShard", () => {
        // Start shard draining with removeShard
        assert.commandWorked(this.st.s.adminCommand({removeShard: this.rs1.name}));

        // Check draining status with shardDrainingStatus
        const shardDrainingStatus = this.st.s.adminCommand({shardDrainingStatus: this.rs1.name});
        assert.commandWorked(shardDrainingStatus);

        // Check draining status with removeShard
        const removeShardStatus = this.st.s.adminCommand({removeShard: this.rs1.name});
        assert.commandWorked(removeShardStatus);

        assert.eq(shardDrainingStatus.state, removeShardStatus.state);
        assert.eq(shardDrainingStatus.msg, removeShardStatus.msg);
        assert.eq(shardDrainingStatus.dbsToMove, removeShardStatus.dbsToMove);
        assert.eq(shardDrainingStatus.note, removeShardStatus.note);
        assert.eq(shardDrainingStatus.collectionsToMove, removeShardStatus.collectionsToMove);
    });
});
