/**
 * Test that shardDrainingStatus works correctly.
 * @tags: [
 * requires_fcv_83
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("commitShardRemoval correct functionality test", function() {
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
        // Stop draining the shard
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
    it("check shard is removed correctly", () => {
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: this.rs1.name}));

        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: "TestDB", to: this.st.shard0.shardName}));

        assert.soon(() => {
            const drainingStatus_completed =
                this.st.s.adminCommand({shardDrainingStatus: this.rs1.name});
            assert.commandWorked(drainingStatus_completed);
            return 'drainingComplete' == drainingStatus_completed.state;
        }, "shardDrainingStatus did not return 'drainingCompleted' status within the timeout.");

        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: this.rs1.name}));

        assert.eq(0, this.st.s.getDB("config").shards.find({_id: this.rs1.name}).toArray().length);

        // Restart the replica set and add it back to the cluster
        this.rs1.stopSet();
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();
        assert.commandWorked(
            this.st.s.adminCommand({addShard: this.rs1.getURL(), name: this.rs1.name}));
    });
    it("can't remove non existent shard", () => {
        assert.commandFailedWithCode(this.st.s.adminCommand({commitShardRemoval: "shard1"}),
                                     ErrorCodes.ShardNotFound);
    });
    it("can't remove non-draining shard", () => {
        assert.commandFailedWithCode(this.st.s.adminCommand({commitShardRemoval: this.rs1.name}),
                                     ErrorCodes.ConflictingOperationInProgress);
    });
    it("can't remove a shard that's not completely drained", () => {
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: this.rs1.name}));

        assert.commandFailedWithCode(this.st.s.adminCommand({commitShardRemoval: this.rs1.name}),
                                     ErrorCodes.IllegalOperation);
    });
});
