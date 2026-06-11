/**
 * Checks that splitChunk, mergeChunks and mergeAllChunksOnShard are rejected when chunk operations
 * are turned off through the allowMigrations flag. Also checks that retrying the same operation
 * while turned off is accepted as a no-op.
 *
 * Coverage for the allowChunkOperations flag lives in chunk_operations_honor_allow_chunk_operations.js.
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

        this.authoritativeShardsDDLEnabled = FeatureFlagUtil.isEnabled(
            this.st.configRS.getPrimary().getDB("admin"),
            "featureFlagAuthoritativeShardsDDL",
        );

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created;
        // otherwise it returns early before reaching the check this test exercises.
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
        // Fresh namespace per test so chunk state and the flag are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
    });

    afterEach(() => {
        // Re-enable chunk operations before dropping, so the collection is left in its default
        // state, then drop it.
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

        // Re-enabling and retrying must succeed, proving the flag alone caused the failure.
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
        // The authoritative mergeAllChunksOnShard coordinator honors allowMigrations; the legacy
        // config-server path does not (only commit splitChunk and mergeChunks do), so this case only
        // applies when the authoritative model is enabled.
        if (!this.authoritativeShardsDDLEnabled) {
            jsTest.log.info("Skipping with feature flag disabled");
            return;
        }

        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

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

    it("allows a retried splitChunk when allowMigrations is false", () => {
        assert.eq(1, this.countChunks(this.ns));

        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));

        this.setAllowMigrations(this.ns, false);

        // Repeating the exact same split while disabled is accepted as a no-op.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.eq(2, this.countChunks(this.ns));

        // A different split still fails.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({split: this.ns, middle: {x: 10}}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(2, this.countChunks(this.ns), "split must not have committed");

        this.setAllowMigrations(this.ns, true);
    });

    it("allows a retried mergeChunks when allowMigrations is false", () => {
        // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.eq(3, this.countChunks(this.ns));

        assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
        assert.eq(2, this.countChunks(this.ns));

        this.setAllowMigrations(this.ns, false);

        // Repeating the exact same merge while disabled is accepted as a no-op.
        assert.commandWorked(this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}));
        assert.eq(2, this.countChunks(this.ns));

        // A different merge still fails.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: MaxKey}]}),
            ErrorCodes.ConflictingOperationInProgress,
        );
        assert.eq(2, this.countChunks(this.ns), "mergeChunks must not have committed");

        this.setAllowMigrations(this.ns, true);
    });
});
