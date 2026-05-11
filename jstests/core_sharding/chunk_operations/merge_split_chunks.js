/**
 * Tests that merge, split and move chunks via mongos works/doesn't work with different chunk
 * configurations. Covers the three split modes (middle, find, bounds), merge across various
 * chunk layouts, and move of newly split chunks.
 *
 * @tags: [
 *  requires_getmore,
 *  assumes_balancer_off,
 *  does_not_support_stepdowns,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 *  requires_2_or_more_shards,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

describe("merge, split, and move chunks", function () {
    const dbName = db.getName();
    const admin = db.getSiblingDB("admin");
    const config = db.getSiblingDB("config");
    const collName = jsTestName();
    const ns = dbName + "." + collName;
    const coll = db.getCollection(collName);
    const shardNames = db.adminCommand({listShards: 1}).shards.map((shard) => shard._id);
    let numDocs;

    before(function () {
        assert.commandWorked(admin.runCommand({enableSharding: dbName}));
        assert.commandWorked(admin.runCommand({shardCollection: ns, key: {_id: 1}}));

        // Create ranges MIN->0, 0->10, (hole), 20->40, 40->50, 50->90, (hole),
        // 100->110, 110->MAX on shard 0, with chunks [10->20) and [90->100) on shard 1.
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 20}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 40}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 50}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 90}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 100}}));
        assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 110}}));

        assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 10}, to: shardNames[1]}));
        assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 90}, to: shardNames[1]}));

        numDocs = 0;
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i <= 120; i++) {
            bulk.insert({_id: i});
            numDocs++;
        }
        assert.commandWorked(bulk.execute({w: "majority"}));

        // S0: min->0, 0->10, 20->40, 40->50, 50->90, 100->110, 110->max
        // S1: 10->20, 90->100
        assert.eq(9, findChunksUtil.findChunksByNs(config, ns).itcount());
    });

    after(function () {
        coll.drop();
    });

    describe("operations on unsharded collections", function () {
        it("rejects split on unsharded collection", function () {
            const unsplittableName = collName + "_unsplittable";
            const unsplittableNs = dbName + "." + unsplittableName;
            assert.commandWorked(db.runCommand({create: unsplittableName}));
            assert.commandFailedWithCode(
                admin.runCommand({split: unsplittableNs, middle: {_id: 0}}),
                ErrorCodes.NamespaceNotSharded,
            );
            db.getCollection(unsplittableName).drop();
        });

        it("rejects merge on unsharded collection", function () {
            const unsplittableName = collName + "_unsplittable_merge";
            const unsplittableNs = dbName + "." + unsplittableName;
            assert.commandWorked(db.runCommand({create: unsplittableName}));
            assert.commandFailedWithCode(
                admin.runCommand({mergeChunks: unsplittableNs, bounds: [{_id: 90}, {_id: MaxKey}]}),
                ErrorCodes.NamespaceNotSharded,
            );
            db.getCollection(unsplittableName).drop();
        });
    });

    // The tests in this block are sequential: each builds on the chunk layout left by the
    // previous test. mochalite runs tests serially within a describe, so this is safe.
    describe("chunk merge and split workflows", function () {
        it("merges chunks including MinKey", function () {
            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: MinKey}, {_id: 10}]}));
            assert.eq(8, findChunksUtil.findChunksByNs(config, ns).itcount());
            // S0: min->10, 20->40, 40->50, 50->90, 100->110, 110->max
            // S1: 10->20, 90->100
        });

        it("merges three adjacent chunks in the middle", function () {
            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 20}, {_id: 90}]}));
            assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
            assert.eq(numDocs, coll.find().itcount());
            // S0: min->10, 20->90, 100->110, 110->max
            // S1: 10->20, 90->100
        });

        it("splits a merged chunk with middle", function () {
            assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 55}}));
            assert.eq(7, findChunksUtil.findChunksByNs(config, ns).itcount());
            assert.eq(numDocs, coll.find().itcount());
            // S0: min->10, 20->55, 55->90, 100->110, 110->max
            // S1: 10->20, 90->100
        });

        it("moves newly split chunks to another shard", function () {
            assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 20}, to: shardNames[1]}));
            assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 55}, to: shardNames[1]}));
            assert.eq(7, findChunksUtil.findChunksByNs(config, ns).itcount());
            assert.eq(numDocs, coll.find().itcount());
            // S0: min->10, 100->110, 110->max
            // S1: 10->20, 20->55, 55->90, 90->100
        });

        it("merges chunks including MaxKey", function () {
            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 100}, {_id: MaxKey}]}));
            assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
            // S0: min->10, 100->max
            // S1: 10->20, 20->55, 55->90, 90->100
        });

        it("merges chunks after a chunk has been moved out of a shard", function () {
            assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 110}, to: shardNames[1]}));
            assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 10}, to: shardNames[0]}));
            assert.eq(numDocs, coll.find().itcount());
            assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
            // S0: min->10, 10->20
            // S1: 20->55, 55->90, 90->100, 100->max

            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 90}, {_id: MaxKey}]}));
            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 20}, {_id: 90}]}));
            assert.eq(numDocs, coll.find().itcount());
            // S0: min->10, 10->20
            // S1: 20->90, 90->max
        });

        it("splits after merge then moves across shards", function () {
            assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 15}}));
            assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 30}}));
            assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 30}, to: shardNames[0]}));
            assert.eq(numDocs, coll.find().itcount());
            // S0: min->10, 10->15, 15->20, 30->90
            // S1: 20->30, 90->max
        });

        it("rejects merge across a hole in chunk ranges", function () {
            // S0 has chunks min->10, 10->15, 15->20, 30->90 with a hole at [20, 30)
            assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{_id: MinKey}, {_id: 90}]}));
            assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
        });

        // The following tests exercise split modes not covered by the workflow above.
        // State at this point:
        // S0: min->10, 10->15, 15->20, 30->90
        // S1: 20->30, 90->max

        it("splits using find mode", function () {
            // Split the [30, 90) chunk on S0 via find (which uses selectMedianKey /
            // splitVector with force:true to auto-select a split point).
            const chunksBefore = findChunksUtil.findChunksByNs(config, ns).itcount();
            assert.commandWorked(admin.runCommand({split: ns, find: {_id: 50}}));
            assert.gt(findChunksUtil.findChunksByNs(config, ns).itcount(), chunksBefore);
            assert.eq(numDocs, coll.find().itcount());
        });

        it("splits using bounds mode", function () {
            // Split the [90, MaxKey) chunk on S1 via bounds (which auto-selects a split
            // point using splitVector on the target chunk).
            const targetChunk = findChunksUtil.findOneChunkByNs(config, ns, {min: {_id: 90}});
            assert.neq(null, targetChunk);
            const chunksBefore = findChunksUtil.findChunksByNs(config, ns).itcount();

            assert.commandWorked(admin.runCommand({split: ns, bounds: [targetChunk.min, targetChunk.max]}));
            assert.gt(findChunksUtil.findChunksByNs(config, ns).itcount(), chunksBefore);
            assert.eq(numDocs, coll.find().itcount());
        });

        it("re-splits at the same point after merge", function () {
            // Split [10, 15) at _id: 12, merge the halves back, then split at 12 again.
            // Confirms the split/merge/split round-trip works.
            const chunksBefore = findChunksUtil.findChunksByNs(config, ns).itcount();

            assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 12}}));
            assert.eq(chunksBefore + 1, findChunksUtil.findChunksByNs(config, ns).itcount());

            assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 10}, {_id: 15}]}));
            assert.eq(chunksBefore, findChunksUtil.findChunksByNs(config, ns).itcount());

            assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 12}}));
            assert.eq(chunksBefore + 1, findChunksUtil.findChunksByNs(config, ns).itcount());
            assert.eq(numDocs, coll.find().itcount());
        });
    });
});
