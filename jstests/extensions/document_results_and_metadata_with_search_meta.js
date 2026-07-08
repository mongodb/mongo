/**
 * Integration tests for $_internalDocumentResultsAndMetadata with metadata binding ($$SEARCH_META).
 *
 * Uses $extensionMultiStream with a "meta" field to exercise the two-stream path through the
 * Exchange router and $$SEARCH_META variable binding.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {
    expandEachPerShard,
    expandPerShard,
    getDrmShardInfo,
    makeCountMergeSetup,
    setupDrmCollection,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

const expectedMeta = {
    count: {lowerBound: 42},
    facets: {
        category: [
            {value: "books", count: 10},
            {value: "movies", count: 32},
        ],
    },
};

describe("$_internalDocumentResultsAndMetadata with $$SEARCH_META binding", function () {
    let coll;
    // The source extension runs on every shard that owns data for the collection. All shards return
    // identical metadata, so the default merge pipeline produces expectedMeta. Tests with per-shard
    // metadata variation supply an explicit mergePipeline.
    let nShards;
    let shardIds;
    let isSharded;

    before(function () {
        coll = db[jsTestName()];
        setupDrmCollection(db, coll);
        ({nShards, shardIds, isSharded} = getDrmShardInfo(db, coll));
    });

    it("returns docs and correct faceted metadata via $$SEARCH_META", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = [
            {_id: 0, name: "doc_0", meta: expectedMeta},
            {_id: 1, name: "doc_1", meta: expectedMeta},
            {_id: 2, name: "doc_2", meta: expectedMeta},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("allows accessing nested $$SEARCH_META fields in $project", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $project: {
                        name: 1,
                        lowerBound: "$$SEARCH_META.count.lowerBound",
                        categories: "$$SEARCH_META.facets.category",
                    },
                },
            ])
            .toArray();
        const categories = expectedMeta.facets.category;
        const perShardDocs = [
            {_id: 0, name: "doc_0", lowerBound: 42, categories},
            {_id: 1, name: "doc_1", lowerBound: 42, categories},
            {_id: 2, name: "doc_2", lowerBound: 42, categories},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("filters with $match on $$SEARCH_META field and projects metadata", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$match: {$expr: {$gt: ["$$SEARCH_META.count.lowerBound", 0]}}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = [
            {_id: 0, name: "doc_0", meta: expectedMeta},
            {_id: 1, name: "doc_1", meta: expectedMeta},
            {_id: 2, name: "doc_2", meta: expectedMeta},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("returns empty result set when source produces metadata but 0 docs", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 0, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result, [], {result});
    });

    it("projects only $$SEARCH_META with no other fields", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$project: {_id: 0, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = Array(3).fill({meta: expectedMeta});
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("returns all docs across getMore batches with metadata bound on every doc", function () {
        const numDocs = 150;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            name: `doc_${i}`,
            meta: expectedMeta,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("returns all docs when the document stream exceeds the Exchange buffer", function () {
        const numDocs = 40;
        const docPad = 512 * 1024;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs, docPad, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            name: `doc_${i}`,
            meta: expectedMeta,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("[sharded] globally merge-sorts doc results by score across shards", function () {
        if (!isSharded) return;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 5, meta: expectedMeta}},
                {$project: {_id: 0, score: 1, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = [
            {score: 5, name: "doc_0", meta: expectedMeta},
            {score: 4, name: "doc_1", meta: expectedMeta},
            {score: 3, name: "doc_2", meta: expectedMeta},
            {score: 2, name: "doc_3", meta: expectedMeta},
            {score: 1, name: "doc_4", meta: expectedMeta},
        ];
        // The source already emits docs in descending score order on each shard, so the global
        // merge by score yields each per-shard doc nShards consecutive times.
        const expected = expandEachPerShard(nShards, perShardDocs);
        assert.eq(result, expected, {result, nShards});
    });

    it("[sharded] merge-sorts uneven per-shard distributions correctly", function () {
        if (nShards < 2) return;
        // Make shard 0 produce 10 docs, shard 1 produce 2 docs, by keying byShard on shardId.
        const byShard = {[shardIds[0]]: {numDocs: 10}, [shardIds[1]]: {numDocs: 2}};
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 0, meta: expectedMeta, byShard}},
                {$project: {_id: 0, score: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const shard0Docs = Array.from({length: 10}, (_, i) => ({
            score: 10 - i,
            meta: expectedMeta,
        }));
        const shard1Docs = Array.from({length: 2}, (_, i) => ({score: 2 - i, meta: expectedMeta}));
        const expected = [...shard0Docs, ...shard1Docs].sort((a, b) => b.score - a.score);
        assert.eq(result, expected, {result});
    });

    it("[sharded] handles one shard producing 0 documents", function () {
        if (!isSharded) return;
        const byShard = {[shardIds[0]]: {numDocs: 5}, [shardIds[1]]: {numDocs: 0}};
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 0, meta: expectedMeta, byShard}},
                {$project: {_id: 0, score: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const expected = [
            {score: 5, meta: expectedMeta},
            {score: 4, meta: expectedMeta},
            {score: 3, meta: expectedMeta},
            {score: 2, meta: expectedMeta},
            {score: 1, meta: expectedMeta},
        ];
        assert.eq(result, expected, {result});
    });

    it("[sharded] router re-sorts merged results by name with $sort", function () {
        if (!isSharded) return;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 5, meta: expectedMeta}},
                {$sort: {name: 1}},
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = [
            {name: "doc_0", meta: expectedMeta},
            {name: "doc_1", meta: expectedMeta},
            {name: "doc_2", meta: expectedMeta},
            {name: "doc_3", meta: expectedMeta},
            {name: "doc_4", meta: expectedMeta},
        ];
        // After global $sort name:1, every per-shard doc appears nShards consecutive times.
        const expected = expandEachPerShard(nShards, perShardDocs);
        assert.eq(result, expected, {result, nShards});
    });

    it("[sharded] pushes $limit to shards and merges to global top-N", function () {
        if (!isSharded) return;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 5, meta: expectedMeta}},
                {$limit: 5},
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        // $limit is unordered relative to shard origin; only the count and meta are deterministic.
        assert.eq(result.length, 5, {result});
        for (const doc of result) {
            assert.docEq(doc.meta, expectedMeta, {doc});
        }
    });

    it("[sharded] aggregates per-shard metadata via $group merge pipeline", function () {
        if (nShards < 2) return;
        // Each shard emits a different per-shard metadata document. The merge pipeline groups them
        // into a single summed-count metadata document for $$SEARCH_META.
        const {byShard, mergePipeline, expectedMeta: mergedMeta} = makeCountMergeSetup(shardIds);
        const result = coll
            .aggregate([
                {
                    $extensionMultiStream: {
                        numDocs: 3,
                        // Top-level meta is required so DRM is configured with metadata; per-shard
                        // overrides supply the actual values returned from each shard.
                        meta: {count: {lowerBound: 0}},
                        byShard,
                        mergePipeline,
                    },
                },
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = [
            {name: "doc_0", meta: mergedMeta},
            {name: "doc_1", meta: mergedMeta},
            {name: "doc_2", meta: mergedMeta},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("[sharded] aggregates metadata via a mongot-shaped $group merge pipeline", function () {
        if (nShards < 2) return;
        // Simulate the metaPipeline mongot emits during planning: a $group on a composite
        // {type, path, bucket} key that $sums a per-shard meta value. Each shard emits one meta
        // doc sharing the same key, so the merge sums them into a single document for $$SEARCH_META
        // (DRM requires the merged meta pipeline to produce exactly one document).
        const byShard = {
            [shardIds[0]]: {meta: {type: "facet", path: "category", bucket: "books", metaVal: 10}},
            [shardIds[1]]: {meta: {type: "facet", path: "category", bucket: "books", metaVal: 32}},
        };
        // This $group query mirrors what mongot hands the router during the planning phase.
        const mergePipeline = [
            {
                $group: {
                    _id: {type: "$type", path: "$path", bucket: "$bucket"},
                    value: {$sum: "$metaVal"},
                },
            },
        ];
        const result = coll
            .aggregate([
                {
                    $extensionMultiStream: {
                        numDocs: 3,
                        // Top-level meta configures DRM; per-shard overrides supply the values.
                        meta: {type: "facet", path: "category", bucket: "books", metaVal: 0},
                        byShard,
                        mergePipeline,
                    },
                },
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const mergedMeta = {_id: {type: "facet", path: "category", bucket: "books"}, value: 42};
        const perShardDocs = Array.from({length: 3}, (_, i) => ({
            name: `doc_${i}`,
            meta: mergedMeta,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("[sharded] default merge pipeline unions metadata fields across shards", function () {
        if (nShards < 2) return;
        // Give two shards disjoint slices of the metadata document and supply no explicit
        // mergePipeline. The default ($group + $mergeObjects + $replaceRoot) must incorporate a
        // field from each shard, proving it unions across shards rather than picking one shard's
        // doc. Any additional shards return the full top-level meta, which the union absorbs.
        const byShard = {
            [shardIds[0]]: {meta: {count: expectedMeta.count}},
            [shardIds[1]]: {meta: {facets: expectedMeta.facets}},
        };
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta, byShard}},
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        const perShardDocs = Array.from({length: 3}, (_, i) => ({
            name: `doc_${i}`,
            meta: expectedMeta,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("allows accessing $$SEARCH_META inside a $facet subpipeline", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $facet: {
                        meta: [{$replaceWith: "$$SEARCH_META"}, {$limit: 1}],
                        docs: [{$project: {name: 1}}],
                    },
                },
            ])
            .toArray();
        const perShardDocs = [
            {_id: 0, name: "doc_0"},
            {_id: 1, name: "doc_1"},
            {_id: 2, name: "doc_2"},
        ];
        const expectedDocs = expandPerShard(nShards, perShardDocs);
        assert.eq(result.length, 1, {result});
        assert.docEq(result[0].meta, [expectedMeta], {result});
        assert.sameMembers(result[0].docs, expectedDocs, {result, nShards});
    });
});
