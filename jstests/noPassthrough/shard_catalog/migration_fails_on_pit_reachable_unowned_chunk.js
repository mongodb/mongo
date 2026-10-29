/**
 * A chunk migration onto a shard must be rejected while that shard's authoritative local catalog
 * (config.shard.catalog.chunks) still holds a chunk that is owned by another shard, records the
 * recipient as a recent owner still reachable by point-in-time (PIT) reads, and extends beyond the
 * span of the donor's source chunk (the chunk that encloses the range being migrated, which the
 * migration commit refreshes). Receiving the range in that state would drop the ownership history
 * for the uncovered portion and corrupt PIT filtering-metadata reads.
 *
 * The donor sends its enclosing source chunk in the _recvChunkStart request, and the recipient
 * checks it at the start of the migration. The distinction is:
 *   - If the stale entry extends beyond the source chunk span (e.g. moving a split portion back so
 *     the source chunk is narrower than the pre-split entry the recipient still holds), the
 *     migration is rejected.
 *   - If the source chunk span fully covers the stale entry (moving the exact same chunk back, or a
 *     merged chunk back over narrower stale sub-ranges), the migration is allowed: the committed
 *     chunk(s) re-insert the whole range with fresh history, so nothing is lost.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Chunk migration onto a shard with a PIT-reachable unowned overlapping chunk", function () {
    before(() => {
        this.st = new ShardingTest({
            shards: 2,
            other: {
                rsOptions: {
                    setParameter: {
                        // Keep a just-donated chunk's onCurrentShardSince above the storage engine's
                        // oldest timestamp (~stable - window) so the stale entry stays PIT-reachable
                        // for the whole test without any waiting.
                        minSnapshotHistoryWindowInSeconds: 3600,
                        // Let the donor's post-migration range deletion run promptly so the
                        // move-back is not blocked draining conflicting deletions.
                        orphanCleanupDelaySecs: 0,
                    },
                },
            },
        });
        // No balancer-driven migrations or auto-merges interfering with the crafted sequence.
        this.st.stopBalancer();

        this.shard0Name = this.st.shard0.shardName;
        this.shard1Name = this.st.shard1.shardName;
        this.configChunks = this.st.s.getDB("config").chunks;
        this.dbCounter = 0;
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Unique db per case so catalog state does not leak between cases.
        this.dbName = `${jsTestName()}_${this.dbCounter++}`;
        this.ns = `${this.dbName}.coll`;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.shard0Name}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        this.collUuid = this.st.s.getDB("config").collections.findOne({_id: this.ns}).uuid;
    });

    // Asserts that a moveChunk covering 'find' onto 'toShard' is rejected by the PIT-reachability
    // blocker: the donor surfaces the recipient's abort as OperationFailed and the message reports
    // that committing the migration would drop point-in-time reachable ownership history.
    const moveChunkExpectRejected = (find, toShard) => {
        const res = this.st.s.adminCommand({moveChunk: this.ns, find, to: toShard});
        assert.commandFailedWithCode(res, ErrorCodes.OperationFailed);
        assert(
            res.errmsg.includes("point-in-time reachable ownership history"),
            "expected the failure to mention the PIT-reachable overlapping chunk",
            {res},
        );
    };

    // Like moveChunkExpectRejected, but issues a moveRange with the given 'bounds' ({min}, {max}, or
    // both). moveRange takes a shard name in 'toShard' (moveChunk's 'to' also accepts a URL).
    const moveRangeExpectRejected = (bounds, toShard) => {
        const res = this.st.s.adminCommand({moveRange: this.ns, ...bounds, toShard});
        assert.commandFailedWithCode(res, ErrorCodes.OperationFailed);
        assert(
            res.errmsg.includes("point-in-time reachable ownership history"),
            "expected the failure to mention the PIT-reachable overlapping chunk",
            {res},
        );
    };

    // Asserts every chunk of the test collection matching 'query' is owned by 'expectedShard'.
    const assertChunksOwnedBy = (query, expectedShard) => {
        const chunks = this.configChunks.find({uuid: this.collUuid, ...query}).toArray();
        assert.gt(chunks.length, 0, {msg: "expected at least one matching chunk", query});
        for (const chunk of chunks) {
            assert.eq(expectedShard, chunk.shard, {chunk});
        }
    };

    it("allows moving the exact same chunk back to its original shard", () => {
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // The donor's source chunk [MinKey, MaxKey) exactly covers shard0's stale [MinKey, MaxKey)
        // entry, so the committed chunk re-inserts the whole range with fresh history and nothing is
        // lost. The move-back is allowed.
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard0Name}),
        );

        assertChunksOwnedBy({}, this.shard0Name);
    });

    it("rejects moving a split portion back to its original shard", () => {
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // Split the moved chunk on shard1. shard0's local shard catalog is not refreshed by the
        // split, so it still holds the pre-split [MinKey, MaxKey) entry owned by shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));

        // The donor's source chunk for this move is only [0, MaxKey), but shard0's stale
        // [MinKey, MaxKey) entry extends below it (down to MinKey). That uncovered portion would
        // lose its history, so the move-back is rejected.
        moveChunkExpectRejected({x: 50}, this.shard0Name);

        assertChunksOwnedBy({min: {x: 0}}, this.shard1Name);
    });

    it("allows moving a merged chunk back to its original shard", () => {
        // Start with two chunks on shard0, then move both to shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: -50}, to: this.shard1Name}),
        );
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 50}, to: this.shard1Name}),
        );

        // Merge the two chunks on shard1. shard0 still holds both pre-merge sub-range entries, each
        // owned by shard1 and still PIT-reachable.
        assert.commandWorked(
            this.st.s.adminCommand({
                mergeChunks: this.ns,
                bounds: [{x: MinKey}, {x: MaxKey}],
            }),
        );

        // The donor's source chunk is the merged [MinKey, MaxKey), which fully covers both of
        // shard0's narrower stale sub-range entries. The committed chunk refreshes the whole range,
        // so the move-back is allowed.
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard0Name}),
        );

        assertChunksOwnedBy({}, this.shard0Name);
    });

    it("allows an initial migration to a shard that never owned the range", () => {
        // Positive control: shard1 has no PIT-reachable history for this range, so the blocker must
        // not fire and the migration succeeds.
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        assertChunksOwnedBy({}, this.shard1Name);
    });

    it("allows moveRange (min only) back when the enclosing source chunk covers the stale entry", () => {
        // Donate the whole [MinKey, MaxKey) chunk to shard1; shard0 keeps a stale [MinKey, MaxKey)
        // entry owned by shard1.
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // moveRange with only 'min' moves [0, MaxKey) back. shard1 still owns the whole
        // [MinKey, MaxKey) chunk, so the donor splits it as part of the migration and reports the
        // enclosing [MinKey, MaxKey) as the source chunk, which covers shard0's stale entry. The
        // move is allowed and the side-split [MinKey, 0) is refreshed in place.
        assert.commandWorked(
            this.st.s.adminCommand({moveRange: this.ns, min: {x: 0}, toShard: this.shard0Name}),
        );

        assertChunksOwnedBy({min: {x: 0}}, this.shard0Name);
        assertChunksOwnedBy({max: {x: 0}}, this.shard1Name);
    });

    it("rejects moveRange (min only) of a split portion back to its original shard", () => {
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // Split on shard1 so [0, MaxKey) becomes its own chunk. shard0 is not refreshed and still
        // holds the pre-split [MinKey, MaxKey) entry owned by shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));

        // moveRange with only 'min' now moves the existing [0, MaxKey) chunk, so the source chunk is
        // [0, MaxKey). shard0's stale [MinKey, MaxKey) entry extends below it, so the move is
        // rejected.
        moveRangeExpectRejected({min: {x: 0}}, this.shard0Name);

        assertChunksOwnedBy({min: {x: 0}}, this.shard1Name);
    });

    it("allows moveRange (max only) back when the enclosing source chunk covers the stale entry", () => {
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // moveRange with only 'max' moves [MinKey, 0) back. shard1 still owns the whole
        // [MinKey, MaxKey) chunk, so the donor splits it and reports the enclosing [MinKey, MaxKey)
        // as the source chunk, which covers shard0's stale entry, so the move is allowed.
        assert.commandWorked(
            this.st.s.adminCommand({moveRange: this.ns, max: {x: 0}, toShard: this.shard0Name}),
        );

        assertChunksOwnedBy({max: {x: 0}}, this.shard0Name);
        assertChunksOwnedBy({min: {x: 0}}, this.shard1Name);
    });

    it("rejects moveRange (max only) of a split portion back to its original shard", () => {
        assert.commandWorked(
            this.st.s.adminCommand({moveChunk: this.ns, find: {x: 0}, to: this.shard1Name}),
        );

        // Split on shard1 so [MinKey, 0) becomes its own chunk. shard0 still holds the pre-split
        // [MinKey, MaxKey) entry owned by shard1.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));

        // moveRange with only 'max' now moves the existing [MinKey, 0) chunk, so the source chunk is
        // [MinKey, 0). shard0's stale [MinKey, MaxKey) entry extends above it, so the move is
        // rejected.
        moveRangeExpectRejected({max: {x: 0}}, this.shard0Name);

        assertChunksOwnedBy({max: {x: 0}}, this.shard1Name);
    });
});
