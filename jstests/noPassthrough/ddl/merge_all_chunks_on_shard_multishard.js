/**
 * Checks that mergeAllChunksOnShard only merges chunks on the named shard: it collapses that
 * shard's contiguous chunks, leaves its non-contiguous chunks alone, and does not touch other
 * shards' chunks. Uses a 3-shard cluster.
 */

import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("mergeAllChunksOnShard scoped per-shard", function () {
    before(() => {
        this.st = new ShardingTest({shards: 3});
        this.dbName = "merge_all_multishard_db";
        this.collName = "coll";
        this.ns = this.dbName + "." + this.collName;

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created.
        configureFailPointForRS(
            this.st.configRS.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );
        configureFailPointForRS(
            this.st.rs0.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );
        configureFailPointForRS(
            this.st.rs1.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );
        configureFailPointForRS(
            this.st.rs2.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        // Pre-split into 7 chunks: [MinKey, 0), [0, 10), [10, 20), [20, 30), [30, 40), [40, 50),
        // [50, MaxKey). All initially live on shard0.
        for (const sp of [0, 10, 20, 30, 40, 50]) {
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: sp}}));
        }

        // Distribute so each shard owns a mix of contiguous and non-contiguous ranges:
        //   shard0: [MinKey, 0), [0, 10), [20, 30)   -- one contiguous pair + one isolated chunk
        //   shard1: [10, 20), [40, 50)                -- two non-contiguous chunks
        //   shard2: [30, 40), [50, MaxKey)            -- two non-contiguous chunks
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.ns,
                find: {x: 15},
                to: this.st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.ns,
                find: {x: 35},
                to: this.st.shard2.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.ns,
                find: {x: 45},
                to: this.st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.ns,
                find: {x: 55},
                to: this.st.shard2.shardName,
            }),
        );

        this.countChunksOnShard = (shardName) =>
            findChunksUtil
                .findChunksByNs(this.st.s.getDB("config"), this.ns, {shard: shardName})
                .itcount();

        // Sanity-check the initial distribution.
        assert.eq(3, this.countChunksOnShard(this.st.shard0.shardName), "shard0 initial chunks");
        assert.eq(2, this.countChunksOnShard(this.st.shard1.shardName), "shard1 initial chunks");
        assert.eq(2, this.countChunksOnShard(this.st.shard2.shardName), "shard2 initial chunks");
    });

    after(() => {
        this.st.stop();
    });

    it("collapses the contiguous pair on shard0 and leaves other shards untouched", () => {
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard0.shardName,
            }),
        );

        // shard0's [MinKey, 0) and [0, 10) merge into [MinKey, 10); [20, 30) has no shard0
        // neighbor, so it stays. 3 chunks become 2.
        const shard0Chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), this.ns, {shard: this.st.shard0.shardName})
            .sort({min: 1})
            .toArray();
        assert.eq(2, shard0Chunks.length, {shard0Chunks});
        assert.docEq({x: MinKey}, shard0Chunks[0].min);
        assert.docEq({x: 10}, shard0Chunks[0].max);
        assert.docEq({x: 20}, shard0Chunks[1].min);
        assert.docEq({x: 30}, shard0Chunks[1].max);

        // shard1 and shard2 are unchanged.
        assert.eq(2, this.countChunksOnShard(this.st.shard1.shardName), "shard1 must be untouched");
        assert.eq(2, this.countChunksOnShard(this.st.shard2.shardName), "shard2 must be untouched");
    });

    it("is a no-op against shard1 (no contiguous shard1-owned pair)", () => {
        const before = this.countChunksOnShard(this.st.shard1.shardName);
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard1.shardName,
            }),
        );
        assert.eq(before, this.countChunksOnShard(this.st.shard1.shardName));
    });

    it("is a no-op against shard2 (no contiguous shard2-owned pair)", () => {
        const before = this.countChunksOnShard(this.st.shard2.shardName);
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard2.shardName,
            }),
        );
        assert.eq(before, this.countChunksOnShard(this.st.shard2.shardName));
    });

    it("after the full sweep, exactly one merge happened (total chunk count = original - 1)", () => {
        // Original total = 7; after the shard0 merge it should be 6 and stay 6.
        const totalChunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), this.ns)
            .itcount();
        assert.eq(6, totalChunks);
    });
});
