/**
 * Test that shardDrainingStatus works correctly.
 * @tags: [
 * requires_fcv_83
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("commitShardRemoval correct functionality test", function() {
    before(() => {
        this.st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
        this.rs1 = new ReplSetTest({name: 'repl1', nodes: 1});
        this.rs2 = new ReplSetTest({name: 'repl2', nodes: 1});
    });

    beforeEach(() => {
        // Restart the replica sets and add them back to the cluster
        // Empty shard
        this.rs1.startSet({shardsvr: ""});
        this.rs1.initiate();
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs1.getURL()}));

        // Shard with data
        this.rs2.startSet({shardsvr: ""});
        this.rs2.initiate();
        assert.commandWorked(this.st.s.adminCommand({addShard: this.rs2.getURL()}));

        // Create DB
        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: 'TestDB', primaryShard: 'repl2'}));

        // Add unsharded collections
        assert.commandWorked(
            this.st.s.getDB("TestDB").CollUnsharded.insert({_id: 1, value: "Pos"}));

        // Start draining the shards
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: 'repl1'}));
        assert.commandWorked(this.st.s.adminCommand({startShardDraining: 'repl2'}));
    });

    afterEach(() => {
        // Clean the data
        assert.commandWorked(this.st.s.getDB('TestDB').dropDatabase());

        removeShard(this.st, 'repl1');
        removeShard(this.st, 'repl2');

        this.rs1.stopSet();
        this.rs2.stopSet();

        // Check that the number of shards remains the same after each test case
        assert.eq(2, this.st.s.getDB("config").shards.count());
    });

    after(() => {
        this.st.stop();
    });
    it("check shard is removed correctly", () => {
        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: 'repl1'}));

        assert.eq(0, this.st.s.getDB("config").shards.find({_id: 'repl1'}).toArray().length);
    });
    it("check command is idempotent", () => {
        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: this.rs1.name}));
        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: this.rs1.name}));
    });
    it("check removing non-existent shard returns ok", () => {
        assert.commandWorked(this.st.s.adminCommand({commitShardRemoval: "shard1"}));
    });
    it("can't remove non-draining shard", () => {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({commitShardRemoval: this.st.shard1.shardName}),
            ErrorCodes.ConflictingOperationInProgress);
    });
    it("can't remove a shard that's not completely drained", () => {
        assert.commandFailedWithCode(this.st.s.adminCommand({commitShardRemoval: 'repl2'}),
                                     ErrorCodes.IllegalOperation);
    });
});
