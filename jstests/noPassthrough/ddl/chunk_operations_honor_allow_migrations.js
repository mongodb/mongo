/**
 * Verifies that splitChunk, mergeChunks and mergeAllChunksOnShard don't succeed when the collection
 * has {allowMigrations: false}.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

describe("commit chunk operations honor allowMigrations under the chunk lock", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2});
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

        this.countChunks = (ns) => findChunksUtil.findChunksByNs(this.st.s.getDB("config"), ns).itcount();
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
        // Re-enable migrations before dropping so the collection document is in the default state
        // for any other tooling that inspects history. Then drop to release resources.
        this.setAllowMigrations(this.ns, true);
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    it("rejects splitChunk when allowMigrations is false", () => {
        assert.eq(1, this.countChunks(this.ns));

        this.setAllowMigrations(this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({split: this.ns, middle: {x: 0}}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(1, this.countChunks(this.ns), "split must not have committed");

        // After re-enabling migrations the same split must succeed, proving the failure was
        // attributable solely to the allowMigrations flag.
        this.setAllowMigrations(this.ns, true);
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));
    });

    it("rejects mergeChunks when allowMigrations is false", () => {
        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

        this.setAllowMigrations(this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(3, this.countChunks(this.ns), "mergeChunks must not have committed");

        this.setAllowMigrations(this.ns, true);
        assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
        assert.eq(2, this.countChunks(this.ns));
    });

    it("rejects mergeAllChunksOnShard when allowMigrations is false", () => {
        // Pre-split into 3 contiguous chunks, all owned by shard0 (the primary).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));
        assert.eq(
            3,
            findChunksUtil
                .findChunksByNs(this.st.s.getDB("config"), this.ns, {shard: this.st.shard0.shardName})
                .itcount(),
        );

        this.setAllowMigrations(this.ns, false);

        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(3, this.countChunks(this.ns), "mergeAllChunksOnShard must not have committed");

        this.setAllowMigrations(this.ns, true);
        assert.commandWorked(this.st.s.adminCommand({mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName}));
        assert.eq(1, this.countChunks(this.ns));
    });
});
