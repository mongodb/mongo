/**
 * Feeds bad inputs to splitChunk, mergeChunks and mergeAllChunksOnShard and checks that each one is
 * rejected cleanly: the command returns an error, config.chunks is unchanged, no critical section
 * is left held, and no coordinator state document is left behind.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Returns the chunks of `ns` as a sorted list of {min, max, shard}, so two snapshots can be
 * compared for equality.
 */
function snapshotChunks(st, ns) {
    return findChunksUtil
        .findChunksByNs(st.s.getDB("config"), ns)
        .sort({min: 1})
        .toArray()
        .map((c) => ({min: c.min, max: c.max, shard: c.shard}));
}

/**
 * Returns true if any shard is holding a critical section on `ns`.
 */
function anyShardHoldsCriticalSection(st, ns) {
    for (let i = 0; i < st._rs.length; i++) {
        const primary = st["rs" + i].getPrimary();
        const found = primary
            .getDB("config")
            .getCollection("collection_critical_sections")
            .findOne({_id: ns});
        if (found !== null) {
            return true;
        }
    }
    return false;
}

/**
 * Returns true if any shard still has a coordinator state document for `ns` of the given type.
 */
function anyCoordinatorStateDocPresent(st, ns, coordinatorType) {
    for (let i = 0; i < st._rs.length; i++) {
        const primary = st["rs" + i].getPrimary();
        const found = primary
            .getDB("config")
            .getCollection("system.sharding_ddl_coordinators")
            .findOne({"_id.namespace": ns, "_id.operationType": coordinatorType});
        if (found !== null) {
            return true;
        }
    }
    return false;
}

/**
 * Checks that a rejected chunk operation left nothing behind: chunks unchanged, no critical section
 * held, and no leftover coordinator document.
 */
function assertNoSideEffects(st, ns, baselineChunks, coordinatorType) {
    assert.eq(
        baselineChunks,
        snapshotChunks(st, ns),
        "config.chunks changed -- rejection left side effects",
    );
    assert(!anyShardHoldsCriticalSection(st, ns), "a critical section was left behind");
    if (coordinatorType !== undefined) {
        assert(
            !anyCoordinatorStateDocPresent(st, ns, coordinatorType),
            "a " + coordinatorType + " coordinator state doc was left behind",
        );
    }
}

describe("authoritative chunk operations reject malformed inputs without side effects", function () {
    before(() => {
        this.st = new ShardingTest({shards: 3});
        this.dbName = "ckprecond_db";

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
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
    });

    afterEach(() => {
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    // ------------------------------------------------------------------------ split

    it("split: empty splitPoints array is rejected", () => {
        const baseline = snapshotChunks(this.st, this.ns);
        // The split command requires a split point, so a request with none is rejected.
        assert.commandFailed(this.st.s.adminCommand({split: this.ns}));
        assertNoSideEffects(this.st, this.ns, baseline, "splitChunk");
    });

    it("split: middle key that does not satisfy the shard-key pattern is rejected", () => {
        const baseline = snapshotChunks(this.st, this.ns);
        // Shard key is `{x: 1}` so a `middle` that lacks `x` is invalid.
        assert.commandFailed(this.st.s.adminCommand({split: this.ns, middle: {y: 0}}));
        assertNoSideEffects(this.st, this.ns, baseline, "splitChunk");
    });

    it("split: a middle key equal to an existing chunk boundary is a no-op", () => {
        // Pre-split so there is an existing boundary at x=0.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        const baseline = snapshotChunks(this.st, this.ns);

        // Splitting again at an existing boundary cannot create a new chunk, so it is a no-op: the
        // command succeeds and the chunks are unchanged.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assertNoSideEffects(this.st, this.ns, baseline, "splitChunk");
    });

    // ------------------------------------------------------------------------ mergeChunks

    it("mergeChunks: bounds enclosing a single chunk are a no-op", () => {
        // Three chunks: [MinKey, 0), [0, 10), [10, MaxKey).
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        const baseline = snapshotChunks(this.st, this.ns);

        // Merging a range that is already a single chunk is a no-op: the command succeeds and the
        // chunks are unchanged.
        assert.commandWorked(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 0}]}),
        );
        assertNoSideEffects(this.st, this.ns, baseline, "mergeChunks");
    });

    it("mergeChunks: bounds that do not align with any chunk boundary are rejected", () => {
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        const baseline = snapshotChunks(this.st, this.ns);

        // 5 is not a chunk boundary.
        assert.commandFailed(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 5}]}),
        );
        assertNoSideEffects(this.st, this.ns, baseline, "mergeChunks");
    });

    it("mergeChunks: bounds spanning chunks on different shards are rejected", () => {
        // Pre-split into three chunks and move the middle one to shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: this.ns,
                find: {x: 5},
                to: this.st.shard1.shardName,
            }),
        );
        const baseline = snapshotChunks(this.st, this.ns);

        // [MinKey, 10) spans chunks on shard0 and shard1, so it cannot be merged.
        assert.commandFailed(
            this.st.s.adminCommand({mergeChunks: this.ns, bounds: [{x: MinKey}, {x: 10}]}),
        );
        assertNoSideEffects(this.st, this.ns, baseline, "mergeChunks");
    });

    // ------------------------------------------------------------ mergeAllChunksOnShard

    it("mergeAllChunksOnShard: against a shard that owns no chunks of the collection is a no-op", () => {
        // All chunks live on shard0, so asking shard1 to merge its chunks of this collection has
        // nothing to do: the command succeeds and the chunks are unchanged.
        const baseline = snapshotChunks(this.st, this.ns);
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: this.ns,
                shard: this.st.shard1.shardName,
            }),
        );
        assert.eq(baseline, snapshotChunks(this.st, this.ns));
        assert.eq(false, anyShardHoldsCriticalSection(this.st, this.ns));
        assert.eq(false, anyCoordinatorStateDocPresent(this.st, this.ns, "mergeAllChunks"));
    });

    it("mergeAllChunksOnShard: non-existent shard is rejected", () => {
        const baseline = snapshotChunks(this.st, this.ns);
        assert.commandFailedWithCode(
            this.st.s.adminCommand({mergeAllChunksOnShard: this.ns, shard: "nonexistent-shard"}),
            ErrorCodes.ShardNotFound,
        );
        assertNoSideEffects(this.st, this.ns, baseline, "mergeAllChunks");
    });

    it("mergeAllChunksOnShard: unsharded namespace is rejected", () => {
        const unshardedNs = this.dbName + ".unsharded_" + new ObjectId().str;
        // Create a regular, unsharded collection.
        assert.commandWorked(
            this.st.s.getDB(this.dbName).createCollection(unshardedNs.split(".")[1]),
        );
        assert.commandFailedWithCode(
            this.st.s.adminCommand({
                mergeAllChunksOnShard: unshardedNs,
                shard: this.st.shard0.shardName,
            }),
            ErrorCodes.NamespaceNotSharded,
        );
        assert.eq(false, anyCoordinatorStateDocPresent(this.st, unshardedNs, "mergeAllChunks"));
    });
});
