/**
 * Integration tests for $_internalDocumentResultsAndMetadata inside $unionWith subpipelines.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {
    getDrmShardInfo,
    kSimpleExpectedMeta,
    makeCountMergeSetup,
    setupDrmCollection,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

const outerMeta = {...kSimpleExpectedMeta, scope: "outer"};
const innerMeta = {count: {lowerBound: 7}, scope: "inner"};

describe("$_internalDocumentResultsAndMetadata in $unionWith", function () {
    let coll;
    let nShards;
    let isSharded;
    let shardIds;

    before(function () {
        coll = db[jsTestName()];
        setupDrmCollection(db, coll);
        ({nShards, shardIds, isSharded} = getDrmShardInfo(db, coll));
    });

    it("unions outer collection scan with DRM subpipeline binding $$SEARCH_META", function () {
        const numDocs = 3;
        const result = coll
            .aggregate([
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$extensionMultiStream: {numDocs, meta: innerMeta}},
                            {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const subDocs = result.filter((d) => d.hasOwnProperty("meta"));
        assert.gt(subDocs.length, 0, {result});
        for (const doc of subDocs) {
            assert.docEq(doc.meta, innerMeta, {doc});
            assert(doc.hasOwnProperty("name"), {doc});
        }
    });

    it("keeps $$SEARCH_META independent in outer DRM and inner $unionWith DRM", function () {
        const outerNum = 2;
        const innerNum = 3;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: outerNum, meta: outerMeta}},
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$extensionMultiStream: {numDocs: innerNum, meta: innerMeta}},
                            {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const outerDocs = result.filter((d) => d.meta && d.meta.scope === "outer");
        const innerDocs = result.filter((d) => d.meta && d.meta.scope === "inner");

        // The exact count of inner docs depends on how the $unionWith subpipeline schedules its
        // DRM source across shards. The invariant we care about here is non-cross-contamination:
        // every doc on each side of the union sees only its own $$SEARCH_META.
        assert.eq(outerDocs.length, outerNum * nShards, {result, outerDocs, nShards});
        assert.gt(innerDocs.length, 0, {result, innerDocs});
        assert.eq(outerDocs.length + innerDocs.length, result.length, {result});
        for (const doc of outerDocs) {
            assert.docEq(doc.meta, outerMeta, {doc});
        }
        for (const doc of innerDocs) {
            assert.docEq(doc.meta, innerMeta, {doc});
        }
    });

    it("rejects $$SEARCH_META reference inside $unionWith subpipeline that has no DRM source", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs: 1, meta: outerMeta}},
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [{$project: {meta: "$$SEARCH_META"}}],
                        },
                    },
                ],
                cursor: {},
            }),
            6347902,
        );
    });

    it("[sharded] merges differing per-shard metadata in a $unionWith DRM subpipeline", function () {
        if (!isSharded || shardIds.length < 2) return;
        const {byShard, mergePipeline, expectedMeta: mergedMeta} = makeCountMergeSetup(shardIds);
        const result = coll
            .aggregate([
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {
                                $extensionMultiStream: {
                                    numDocs: 3,
                                    meta: {count: {lowerBound: 0}},
                                    byShard,
                                    mergePipeline,
                                },
                            },
                            {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const subDocs = result.filter((d) => d.hasOwnProperty("meta"));
        assert.gt(subDocs.length, 0, {result, byShard});
        for (const doc of subDocs) {
            assert.docEq(doc.meta, mergedMeta, {doc, byShard});
            assert(doc.hasOwnProperty("name"), {doc});
        }
    });

    it("[sharded] propagates DRM subpipeline metadata correctly across shards", function () {
        if (!isSharded) return;
        const numDocs = 4;
        const result = coll
            .aggregate([
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$extensionMultiStream: {numDocs, meta: innerMeta}},
                            {$project: {_id: 0, name: 1, score: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const subDocs = result.filter((d) => d.hasOwnProperty("meta"));
        // Sharded $unionWith subpipeline scheduling determines whether the DRM source runs once
        // or per shard; both are correct as long as every produced doc carries the inner metadata.
        assert.gte(subDocs.length, numDocs, {result, nShards});
        for (const doc of subDocs) {
            assert.docEq(doc.meta, innerMeta, {doc});
            assert(doc.hasOwnProperty("name"), {doc});
            assert(doc.hasOwnProperty("score"), {doc});
        }
    });
});
