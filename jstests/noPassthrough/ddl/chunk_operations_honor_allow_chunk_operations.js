/**
 * Checks that splitChunk, mergeChunks and mergeAllChunksOnShard are rejected while chunk operations
 * are disabled through the allowChunkOperations flag, and succeed again once re-enabled. For
 * splitChunk and mergeChunks it also checks that retrying the same operation while disabled is
 * accepted as a no-op. Finally it checks that disabling allowChunkOperations is reflected on both
 * the config server and the per-shard catalog.
 *
 * Coverage for the legacy allowMigrations flag lives in chunk_operations_honor_allow_migrations.js.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    getConfigSvrAllowChunkOperations,
    getShardAllowChunkOperations,
    setAllowChunkOperations,
} from "jstests/sharding/libs/set_allow_chunk_operations_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("commit chunk operations honor allowChunkOperations under the chunk lock", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2});
        this.dbName = "honor_allow_chunk_ops_db";

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created;
        // otherwise it returns early before reaching the check this test exercises.
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

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        this.countChunks = (ns) =>
            findChunksUtil.findChunksByNs(this.st.s.getDB("config"), ns).itcount();
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Fresh namespace per test so chunk state and the flags are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
    });

    afterEach(() => {
        // Re-enable chunk operations so the collection is left in its default state, then drop it.
        // Use the full helper so the per-shard catalog is cleared too.
        setAllowChunkOperations(this.st, this.ns, true);
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    it("propagates the allowChunkOperations flag to both the config server and the per-shard catalog", () => {
        // To start, the flag is on and the config-server doc has no allowChunkOperations field.
        assert.eq(undefined, getConfigSvrAllowChunkOperations(this.st, this.ns));
        assert.eq(undefined, getShardAllowChunkOperations(this.st.shard0, this.ns));

        // setAllowChunkOperations runs the config-server command and the per-shard command, so both
        // layers must observe the change.
        setAllowChunkOperations(this.st, this.ns, false);
        assert.eq(false, getConfigSvrAllowChunkOperations(this.st, this.ns));
        // shard0 owns the only chunk, so it must have received the shard write.
        assert.eq(false, getShardAllowChunkOperations(this.st.shard0, this.ns));

        setAllowChunkOperations(this.st, this.ns, true);
        assert.eq(undefined, getConfigSvrAllowChunkOperations(this.st, this.ns));
        assert.eq(undefined, getShardAllowChunkOperations(this.st.shard0, this.ns));
    });

    // splitChunk
    it("rejects splitChunk when allowChunkOperations is false", () => {
        assert.eq(1, this.countChunks(this.ns));

        setAllowChunkOperations(this.st, this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({split: this.ns, middle: {x: 0}}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(1, this.countChunks(this.ns), "split must not have committed");

        // Re-enabling and retrying must succeed, proving the flag alone caused the failure.
        setAllowChunkOperations(this.st, this.ns, true);
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));
    });

    it("allows a retried splitChunk when allowChunkOperations is false", () => {
        assert.eq(1, this.countChunks(this.ns));

        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));

        setAllowChunkOperations(this.st, this.ns, false);

        // Repeating the exact same split while disabled is accepted as a no-op.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));

        // A different split still fails.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({split: this.ns, middle: {x: 10}}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(2, this.countChunks(this.ns), "split must not have committed");

        setAllowChunkOperations(this.st, this.ns, true);
    });

    // mergeChunks
    it("rejects mergeChunks when allowChunkOperations is false", () => {
        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

        setAllowChunkOperations(this.st, this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(3, this.countChunks(this.ns), "mergeChunks must not have committed");

        setAllowChunkOperations(this.st, this.ns, true);
        assert.commandWorked(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
        );
        assert.eq(2, this.countChunks(this.ns));
    });

    it("allows a retried mergeChunks when allowChunkOperations is false", () => {
        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

        assert.commandWorked(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
        );
        assert.eq(2, this.countChunks(this.ns));

        setAllowChunkOperations(this.st, this.ns, false);

        // Repeating the exact same merge while disabled is accepted as a no-op.
        assert.commandWorked(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
        );
        assert.eq(2, this.countChunks(this.ns));

        // A different merge still fails.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: MaxKey}]}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(2, this.countChunks(this.ns), "mergeChunks must not have committed");

        setAllowChunkOperations(this.st, this.ns, true);
    });

    // mergeAllChunksOnShard. There is no "retried no-op" case here: mergeAllChunksOnShard checks the
    // flag unconditionally, so it is rejected whenever the flag is off, even with nothing to merge.
    it("rejects mergeAllChunksOnShard when allowChunkOperations is false", () => {
        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

        // setAllowChunkOperations writes the per-shard catalog too; the mergeAllChunksOnShard
        // coordinator reads allowChunkOperations from there, not from the config server.
        setAllowChunkOperations(this.st, this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard0.shardName,
            }),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(3, this.countChunks(this.ns), "mergeAllChunksOnShard must not have committed");

        setAllowChunkOperations(this.st, this.ns, true);
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard0.shardName,
            }),
        );
        assert.eq(1, this.countChunks(this.ns));
    });
});
