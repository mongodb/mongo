/**
 * Test that shardDrainingStatus works correctly.
 * @tags: [
 * requires_fcv_82
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("shardDrainingStatus correct functionality test", function() {
    before(() => {
        this.st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
    });

    beforeEach(() => {
        // Add sharded collections
        assert.commandWorked(this.st.s.adminCommand(
            {enableSharding: 'TestDB', primaryShard: this.st.shard1.shardName}));
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: 'TestDB.Coll', key: {_id: 1}}));
        assert.commandWorked(
            this.st.s.getDB('TestDB').Coll.insert({_id: -1, value: 'Negative value'}));
        assert.commandWorked(
            this.st.s.getDB('TestDB').Coll.insert({_id: 1, value: 'Positive value'}));

        // Add unsharded collections
        assert.commandWorked(
            this.st.s.getDB('TestDB').CollUnsharded.insert({_id: 1, value: 'Pos'}));

        // Start shard draining
        assert.commandWorked(
            this.st.s.adminCommand({startShardDraining: this.st.shard1.shardName}));
        this.st.configRS.awaitReplication();
    });

    afterEach(() => {
        // TODO-SERVER-107018: Use stopShardDraining instead of manually unsetting the flag.
        this.st.s.getDB('config').shards.updateOne({_id: this.st.shard1.shardName},
                                                   {$set: {draining: false}});
        // Clean the data
        assert.commandWorked(this.st.s.getDB('TestDB').dropDatabase());
    });

    after(() => {
        this.st.stop();
    });

    it("draining status is 'ongoing", () => {
        const res = this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
        assert.commandWorked(res);
        assert.eq(res.state, 'ongoing');
    });

    it("msg is 'draining ongoing'", () => {
        const res = this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
        assert.commandWorked(res);
        assert.eq(res.msg, 'draining ongoing');
    });

    it("TestDB is listed as db to move", () => {
        const res = this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
        assert.commandWorked(res);
        assert.eq(res.dbsToMove, ["TestDB"]);
    });

    it("note field is set correctly", () => {
        const res = this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
        assert.commandWorked(res);
        assert.eq(
            res.note,
            "you need to call moveCollection for collectionsToMove and afterwards movePrimary for the dbsToMove");
    });

    it("TestDB.Coll is listed as collection to move", () => {
        const res = this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
        assert.commandWorked(res);
        assert.eq(res.collectionsToMove, ["TestDB.CollUnsharded"]);
    });

    it("draining status is 'drainingComplete' when shard is completely drained", () => {
        // Move the unsharded collections
        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: "TestDB", to: this.st.shard0.shardName}));

        // Wait for the shard to be completely drained, then shardDrainingStatus must return
        // completed.
        assert.soon(() => {
            const drainingStatus_completed =
                this.st.s.adminCommand({shardDrainingStatus: this.st.shard1.shardName});
            assert.commandWorked(drainingStatus_completed);
            return 'drainingComplete' == drainingStatus_completed.state;
        }, "shardDrainingStatus did not return 'drainingCompleted' status within the timeout.");
    });

    it("status not returned for non existent shard", () => {
        assert.commandFailedWithCode(this.st.s.adminCommand({shardDrainingStatus: "shard1"}),
                                     ErrorCodes.ShardNotFound);
    });

    it("status not returned for non-draining shard", () => {
        assert.commandFailedWithCode(
            this.st.s.adminCommand({shardDrainingStatus: this.st.shard0.shardName}),
            ErrorCodes.IllegalOperation);
    });
});
