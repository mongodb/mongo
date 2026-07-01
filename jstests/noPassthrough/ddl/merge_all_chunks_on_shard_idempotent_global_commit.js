/**
 * Checks that the global commit command _configSvrCommitMergeAllPrecomputedChunksOnShard is idempotent and succeeds after it returns error.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("mergeAllChunksOnShard global commit is idempotent", function () {
    before(() => {
        // Single node CSRS to make it easier to deal with the failpoint.
        this.st = new ShardingTest({shards: 1, config: 1});
        this.dbName = "merge_all_db";
        this.collName = "coll";
        this.ns = this.dbName + "." + this.collName;

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created.
        configureFailPoint(
            this.st.rs0.getPrimary(),
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

        // Pre-split into 7 chunks: [MinKey, 0), [0, 10), [10, 20), [20, 30), [30, 40), [40, 50), [50, MaxKey).
        for (const sp of [0, 10, 20, 30, 40, 50]) {
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: sp}}));
        }
    });

    after(() => {
        this.st.stop();
    });

    it("succeeds when retrying a global commit", () => {
        // mergeAllChunksFailAfterCommit makes _configSvrCommitMergeAllPrecomputedChunksOnShard always
        // fail after the commit of the transaction. The coordinator will retry and the second time
        // around, the command will simply return through the retry branch.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "mergeAllChunksFailAfterCommit",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard0.shardName,
            }),
        );

        // Check that the command succeeded.
        const shard0Chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), this.ns, {shard: this.st.shard0.shardName})
            .sort({min: 1})
            .toArray();
        assert.eq(1, shard0Chunks.length, {shard0Chunks});
        assert.docEq({x: MinKey}, shard0Chunks[0].min);
        assert.docEq({x: MaxKey}, shard0Chunks[0].max);

        // If wait returns successfully, it means that the fp was actually hit.
        assert(fp.waitWithTimeout(600000)); // 10 minutes
        fp.off();
    });
});
