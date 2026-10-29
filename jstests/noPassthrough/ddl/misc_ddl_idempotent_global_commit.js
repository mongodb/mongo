/**
 * Checks that some miscellaneous operations that have a commit phase against the CSRS can be safely
 * retried. Each operation has a dedicated "fail after commit" failpoint that throws a retryable error
 * once the transaction has been persisted; the retry must detect the already-committed state and
 * return success without re-doing any writes.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("global commit is idempotent", function () {
    before(() => {
        // Single node CSRS to make it easier to deal with the failpoint. Two shards so movePrimary
        // has a destination.
        this.st = new ShardingTest({shards: 2, config: 1});
        this.dbName = "idempotency_db";

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

    it("movePrimary succeeds when retrying a global commit", () => {
        const movedDb = "moved_db";
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: movedDb,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        // commitMovePrimaryFailsAfterDurableChange makes _configsvrCommitMovePrimary
        // always fail after the commit of the transaction. The coordinator will retry and the
        // second time around, the command will simply return through the retry branch.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitMovePrimaryFailsAfterDurableChange",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                movePrimary: movedDb,
                to: this.st.shard1.shardName,
            }),
        );

        // Check that the command succeeded.
        const result = this.st.s.getDB("config").databases.find({_id: movedDb}).toArray();
        assert.eq(result.length, 1);
        assert.eq(result[0].primary, this.st.shard1.shardName);

        // If wait returns successfully, it means that the fp was actually hit.
        assert(fp.waitWithTimeout(600000)); // 10 minutes
        fp.off();
    });

    it("refineShardKey succeeds when retrying a global commit", () => {
        const collName = "refined";
        const ns = shardFreshCollection(collName);

        // commitRefineCollectionShardKeyFailsAfterDurableChange makes the config server fail after the refine transaction
        // is committed. The retry detects the refinement as already done and returns success.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitRefineCollectionShardKeyFailsAfterDurableChange",
        );

        assert.commandWorked(this.st.s.getDB(this.dbName)[collName].createIndex({x: 1, y: 1}));
        assert.commandWorked(
            this.st.s.adminCommand({refineCollectionShardKey: ns, key: {x: 1, y: 1}}),
        );

        const chunks = findChunksUtil
            .findChunksByNs(this.st.s.getDB("config"), ns)
            .sort({min: 1})
            .toArray();
        assert.gt(chunks.length, 0, {chunks});
        const chunk = chunks[0];
        assert.neq(chunk.min.x, undefined);
        assert.neq(chunk.min.y, undefined);
        assert.neq(chunk.max.x, undefined);
        assert.neq(chunk.max.y, undefined);

        assert(fp.waitWithTimeout(600000));
        fp.off();
    });
});
