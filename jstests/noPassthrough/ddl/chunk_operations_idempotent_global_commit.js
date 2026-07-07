/**
 * Checks that the global commit of every chunk operation is idempotent and succeeds after the
 * config server fails right after durably committing the operation's transaction. Each operation
 * has a dedicated "fail after commit" failpoint that throws a retryable error once the transaction
 * has been persisted; the retry must detect the already-committed state and return success without
 * re-doing any writes.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("chunk operation global commit is idempotent", function () {
    before(() => {
        // Single node CSRS to make it easier to deal with the failpoint. Two shards so migration
        // has a destination.
        this.st = new ShardingTest({shards: 2, config: 1});
        this.dbName = "chunk_ops_db";

        // Let merge/mergeAllChunks merge chunks no matter how recently they were created.
        for (const rs of [this.st.configRS, this.st.rs0, this.st.rs1]) {
            configureFailPoint(
                rs.getPrimary(),
                "overrideHistoryWindowInSecs",
                {seconds: -10},
                "alwaysOn",
            );
        }

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
    });

    after(() => {
        this.st.stop();
    });

    // Shards a fresh collection on {x: 1} and returns its namespace. Each test uses its own
    // namespace so cases don't interfere with each other's chunk layout.
    const shardFreshCollection = (collName) => {
        const ns = this.dbName + "." + collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
        return ns;
    };

    it("mergeAllChunksOnShard succeeds when retrying a global commit", () => {
        const ns = shardFreshCollection("merge_all");

        // Pre-split into 7 chunks: [MinKey, 0), [0, 10), [10, 20), [20, 30), [30, 40), [40, 50),
        // [50, MaxKey).
        for (const sp of [0, 10, 20, 30, 40, 50]) {
            assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: sp}}));
        }

        // mergeAllChunksFailAfterCommit makes _configSvrCommitMergeAllPrecomputedChunksOnShard
        // always fail after the commit of the transaction. The coordinator will retry and the
        // second time around, the command will simply return through the retry branch.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "mergeAllChunksFailAfterCommit",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: ns,
                shard: this.st.shard0.shardName,
            }),
        );

        // Check that the command succeeded.
        const shard0Chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), ns, {shard: this.st.shard0.shardName})
            .sort({min: 1})
            .toArray();
        assert.eq(1, shard0Chunks.length, {shard0Chunks});
        assert.docEq({x: MinKey}, shard0Chunks[0].min);
        assert.docEq({x: MaxKey}, shard0Chunks[0].max);

        // If wait returns successfully, it means that the fp was actually hit.
        assert(fp.waitWithTimeout(600000)); // 10 minutes
        fp.off();
    });

    it("split chunk succeeds when retrying a global commit", () => {
        const ns = shardFreshCollection("split");

        // commitChunkSplitFailAfterCommit makes the config server fail after the split transaction
        // is committed. The retry detects the split as already done and returns success.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitChunkSplitFailAfterCommit",
        );

        assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: 0}}));

        const chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), ns)
            .sort({min: 1})
            .toArray();
        assert.eq(2, chunks.length, {chunks});
        assert.docEq({x: MinKey}, chunks[0].min);
        assert.docEq({x: 0}, chunks[0].max);
        assert.docEq({x: 0}, chunks[1].min);
        assert.docEq({x: MaxKey}, chunks[1].max);

        assert(fp.waitWithTimeout(600000));
        fp.off();
    });

    it("merge chunks succeeds when retrying a global commit", () => {
        const ns = shardFreshCollection("merge");

        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        for (const sp of [0, 10]) {
            assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: sp}}));
        }

        // commitChunksMergeFailAfterCommit makes the config server fail after the merge transaction
        // is committed. The retry detects the range as already merged and returns success.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitChunksMergeFailAfterCommit",
        );

        assert.commandWorked(
            this.st.s.adminCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: 10}]}),
        );

        const chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), ns)
            .sort({min: 1})
            .toArray();
        assert.eq(2, chunks.length, {chunks});
        assert.docEq({x: MinKey}, chunks[0].min);
        assert.docEq({x: 10}, chunks[0].max);

        assert(fp.waitWithTimeout(600000));
        fp.off();
    });

    it("migration succeeds when retrying a global commit", () => {
        const ns = shardFreshCollection("migration");

        // commitChunkMigrationFailAfterCommit makes the config server fail after the migration
        // transaction is committed. The retry detects the chunk as already on the recipient and
        // returns success.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitChunkMigrationFailAfterCommit",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: ns,
                min: {x: MinKey},
                toShard: this.st.shard1.shardName,
            }),
        );

        // The entire [MinKey, MaxKey) chunk should now live on shard1.
        const shard1Chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), ns, {shard: this.st.shard1.shardName})
            .toArray();
        assert.eq(1, shard1Chunks.length, {shard1Chunks});
        assert.docEq({x: MinKey}, shard1Chunks[0].min);
        assert.docEq({x: MaxKey}, shard1Chunks[0].max);

        assert(fp.waitWithTimeout(600000));
        fp.off();
    });
});
