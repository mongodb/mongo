/**
 * Integration tests for document-level metadata propagation through
 * $_internalDocumentResultsAndMetadata.
 *
 * Extension metadata plumbing itself is covered by jstests/extensions/metadata.js; this file only
 * exercises that the DRM routing layer preserves it.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const expectedMeta = {
    count: {lowerBound: 42},
};

describe("$_internalDocumentResultsAndMetadata document-level metadata", function () {
    let coll;
    let nShards;
    let isSharded;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        if (FixtureHelpers.isMongos(db)) {
            assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
            assert.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
            );
        } else {
            assert.commandWorked(coll.insertOne({_id: 0}));
        }
        nShards = FixtureHelpers.numberOfShardsForCollection(coll);
        isSharded = FixtureHelpers.isSharded(coll);
    });

    it("projects $meta:searchScore through the DRM payload envelope alongside $$SEARCH_META", function () {
        // The source emits $searchScore as score * 0.125 (exact in IEEE 754, distinct from the
        // integer score field), so a non-trivial assertion proves the metadata BSON pathway.
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $project: {
                        _id: 0,
                        name: 1,
                        searchScore: {$meta: "searchScore"},
                        meta: "$$SEARCH_META",
                    },
                },
            ])
            .toArray();
        // numDocs=3: score values are 3, 2, 1 → $searchScore is 0.375, 0.25, 0.125.
        const perShardDocs = [
            {name: "doc_0", searchScore: 0.375, meta: expectedMeta},
            {name: "doc_1", searchScore: 0.25, meta: expectedMeta},
            {name: "doc_2", searchScore: 0.125, meta: expectedMeta},
        ];
        const expected = Array(nShards).fill(perShardDocs).flat();
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("preserves $meta:searchScore through a downstream $match", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 5, meta: expectedMeta}},
                {$match: {score: {$gt: 3}}},
                {
                    $project: {
                        _id: 0,
                        name: 1,
                        searchScore: {$meta: "searchScore"},
                        meta: "$$SEARCH_META",
                    },
                },
            ])
            .toArray();
        // numDocs=5: score values 5..1, $match keeps score=5 (doc_0) and score=4 (doc_1).
        // $searchScore is score * 0.125, so 0.625 and 0.5 respectively.
        const perShardDocs = [
            {name: "doc_0", searchScore: 0.625, meta: expectedMeta},
            {name: "doc_1", searchScore: 0.5, meta: expectedMeta},
        ];
        const expected = Array(nShards).fill(perShardDocs).flat();
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("[sharded] preserves $meta:searchScore after cross-shard merge-sort", function () {
        if (!isSharded) return;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $project: {
                        _id: 0,
                        name: 1,
                        searchScore: {$meta: "searchScore"},
                        meta: "$$SEARCH_META",
                    },
                },
            ])
            .toArray();
        // The DPL sort pattern is {score: -1}; each per-shard doc appears nShards consecutive
        // times after global merge. $searchScore is score * 0.125.
        const perShardDocs = [
            {name: "doc_0", searchScore: 0.375, meta: expectedMeta},
            {name: "doc_1", searchScore: 0.25, meta: expectedMeta},
            {name: "doc_2", searchScore: 0.125, meta: expectedMeta},
        ];
        const expected = perShardDocs.flatMap((d) => Array(nShards).fill(d));
        assert.eq(result, expected, {result, nShards});
    });
});
