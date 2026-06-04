/**
 * Verifies that splitChunk, mergeChunks and mergeAllChunksOnShard don't succeed when the collection
 * has {allowMigrations: false}.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

describe("commit chunk operations honor allowMigrations under the chunk lock", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.dbName = "split_merge_allow_migrations_db";

        // Make every chunk eligible for mergeAllChunksOnShard regardless of how recently it was
        // produced; otherwise commitMergeAllChunksOnShard short-circuits before reaching the
        // allowMigrations check under test.
        configureFailPointForRS(this.st.configRS.nodes, "overrideHistoryWindowInSecs", {seconds: -10}, "alwaysOn");

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.st.shard0.shardName}),
        );

        this.setAllowMigrations = (ns, allow) => {
            assert.commandWorked(
                this.st.configRS.getPrimary().adminCommand({
                    _configsvrSetAllowMigrations: ns,
                    allowMigrations: allow,
                    writeConcern: {w: "majority"},
                }),
            );
        };

        this.setAllowChunkOperations = (ns, allow) => {
            assert.commandWorked(
                this.st.configRS.getPrimary().adminCommand({
                    _configsvrSetAllowChunkOperations: ns,
                    allowChunkOperations: allow,
                    writeConcern: {w: "majority"},
                }),
            );
        };

        this.countChunks = (ns) => findChunksUtil.findChunksByNs(this.st.s.getDB("config"), ns).itcount();

        this.checkSplitChunk = (setAllowFn) => {
            assert.eq(1, this.countChunks(this.ns));

            setAllowFn.call(this, this.ns, false);

            assert.commandFailedWithCode(
                this.st.s.adminCommand({split: this.ns, middle: {x: 0}}),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(1, this.countChunks(this.ns), "split must not have committed");

            // After re-enabling migrations the same split must succeed, proving the failure was
            // attributable solely to the allowMigrations/allowChunkOperations flag.
            setAllowFn.call(this, this.ns, true);
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.eq(2, this.countChunks(this.ns));
        };

        this.checkMergeChunks = (setAllowFn) => {
            // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
            assert.eq(3, this.countChunks(this.ns));

            setAllowFn.call(this, this.ns, false);

            assert.commandFailedWithCode(
                this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(3, this.countChunks(this.ns), "mergeChunks must not have committed");

            setAllowFn.call(this, this.ns, true);
            assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
            assert.eq(2, this.countChunks(this.ns));
        };

        this.checkMergeAllChunksOnShard = (setAllowFn) => {
            // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
            assert.eq(3, this.countChunks(this.ns));

            setAllowFn.call(this, this.ns, false);

            assert.commandFailedWithCode(
                this.st.s.adminCommand({mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName}),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(3, this.countChunks(this.ns), "mergeAllChunksOnShard must not have committed");

            setAllowFn.call(this, this.ns, true);
            assert.commandWorked(
                this.st.s.adminCommand({mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName}),
            );
            assert.eq(1, this.countChunks(this.ns));
        };

        this.checkSplitChunkIdempotency = (setAllowFn) => {
            assert.eq(1, this.countChunks(this.ns));

            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.eq(2, this.countChunks(this.ns));

            setAllowFn.call(this, this.ns, false);

            // The same split operation under allowChunkOperations = false returns OK.
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.eq(2, this.countChunks(this.ns));

            // A different split still fails.
            assert.commandFailedWithCode(
                this.st.s.adminCommand({split: this.ns, middle: {x: 10}}),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(2, this.countChunks(this.ns), "split must not have committed");

            setAllowFn.call(this, this.ns, true);
        };

        this.checkMergeChunksIdempotency = (setAllowFn) => {
            // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
            assert.eq(3, this.countChunks(this.ns));

            assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
            assert.eq(2, this.countChunks(this.ns));

            setAllowFn.call(this, this.ns, false);

            // The same merge operation under allowChunkOperations = false returns OK.
            assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
            assert.eq(2, this.countChunks(this.ns));

            // A different merge still fails.
            assert.commandFailedWithCode(
                this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: MaxKey}]}),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(2, this.countChunks(this.ns), "mergeChunks must not have committed");

            setAllowFn.call(this, this.ns, true);
        };
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Per-test namespace so chunk state and the allowMigrations flag are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
    });

    afterEach(() => {
        // Re-enable chunk operations before dropping so the collection document is in the default state
        // for any other tooling that inspects history. Then drop to release resources.
        this.setAllowMigrations(this.ns, true);
        this.setAllowChunkOperations(this.ns, true);
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    it("rejects splitChunk when allowMigrations is false", () => {
        this.checkSplitChunk(this.setAllowMigrations);
    });

    it("rejects splitChunk when allowChunkOperations is false", () => {
        this.checkSplitChunk(this.setAllowChunkOperations);
    });

    it("rejects mergeChunks when allowMigrations is false", () => {
        this.checkMergeChunks(this.setAllowMigrations);
    });

    it("rejects mergeAllChunksOnShard when allowMigrations is false", () => {
        this.checkMergeAllChunksOnShard(this.setAllowMigrations);
    });

    it("allows a retried splitChunk when allowMigrations is false", () => {
        this.checkSplitChunkIdempotency(this.setAllowMigrations);
    });

    it("allows a retried splitChunk when allowChunkOperations is false", () => {
        this.checkSplitChunkIdempotency(this.setAllowChunkOperations);
    });
});
