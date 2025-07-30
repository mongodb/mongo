/**
 * Test that stopShardDraining works correctly.
 * @tags: [
 * requires_fcv_82,
 * # TODO(SERVER-108416): Remove exclusion when 8.2 -> 8.3 FCV change finishes
 * multiversion_incompatible,
 * ]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("stopShardDraining correct functionality test", function() {
    before(() => {
        this.st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
    });

    beforeEach(() => {
        assert.commandWorked(
            this.st.s.adminCommand({startShardDraining: this.st.shard1.shardName}));
    });

    after(() => {
        this.st.stop();
    });

    it("check that draining is stopped correctly", () => {
        const config = this.st.s.getDB('config');

        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: this.st.shard1.shardName}));

        // Check that draining has stopped successfully
        const notDrainingShards = config.shards.find({'draining': true}).toArray();

        assert.eq(0, notDrainingShards.length);
    });

    it("check that command returns OK on a non draining shard", () => {
        // stop the draining
        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: this.st.shard1.shardName}));
        // check command returns OK
        assert.commandWorked(this.st.s.adminCommand({stopShardDraining: this.st.shard1.shardName}));
    });

    it("can't stop draining a non existent shard", () => {
        assert.commandFailedWithCode(this.st.s.adminCommand({stopShardDraining: "shard1"}),
                                     ErrorCodes.ShardNotFound);
    });
});
