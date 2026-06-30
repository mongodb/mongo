/**
 * Integration tests for $_internalDocumentResultsAndMetadata explain output.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAggPlanStages,
    getAggStagesAcrossSplitPipeline,
    getSplitPipelineStages,
    getUnionWithStage,
    hasMergeCursors,
} from "jstests/libs/query/analyze_plan.js";
import {
    getDrmShardInfo,
    kSimpleExpectedMeta,
    setupDrmCollection,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

const kDrmStage = "$_internalDocumentResultsAndMetadata";
const kReplaceRootStage = "$replaceRoot";
const kSetVarStage = "$setVariableFromSubPipeline";

const kSourceNoMeta = {$_multiStreamSource: {numDocs: 3}};
const kSourceWithMeta = {$_multiStreamSource: {numDocs: 3, meta: kSimpleExpectedMeta}};
const kDrmSpecNoMeta = {source: kSourceNoMeta, returnCursor: false};
const kDrmSpecWithMeta = {
    source: kSourceWithMeta,
    metadata: {as: "SEARCH_META"},
    returnCursor: false,
};
const kDrmSpecWithMetaSharded = {
    source: kSourceWithMeta,
    metadata: {as: "SEARCH_META"},
    returnCursor: true,
};
const kDrmSpecMetaElided = {source: kSourceWithMeta, returnCursor: false};

function getDrmSpecs(explain) {
    return getAggStagesAcrossSplitPipeline(explain, kDrmStage).map((s) => s[kDrmStage]);
}

function hasSetVarInMerger(explain) {
    return getSplitPipelineStages(explain, kSetVarStage).some((s) => s.part === "mergerPart");
}

describe("$_internalDocumentResultsAndMetadata explain", function () {
    let coll;
    let nShards, isSharded;

    before(function () {
        coll = db[jsTestName()];
        setupDrmCollection(db, coll);
        ({nShards, isSharded} = getDrmShardInfo(db, coll));
    });

    describe("no metadata configured", function () {
        it("[standalone] queryPlanner explain serializes the DRM stage", function () {
            if (isSharded) return;
            const explain = coll
                .explain("queryPlanner")
                .aggregate([{$extensionMultiStream: {numDocs: 3}}]);
            const drmSpecs = getDrmSpecs(explain);
            assert.eq(drmSpecs.length, 1, {explain});
            assert.docEq(kDrmSpecNoMeta, drmSpecs[0], {explain});
        });

        it("executionStats explain reports per-stage execution metrics", function () {
            const explain = coll
                .explain("executionStats")
                .aggregate([{$extensionMultiStream: {numDocs: 3}}]);
            // DRM lowers to Exchange + $replaceRoot; both must surface per-stage execution metrics.
            for (const stageName of [kDrmStage, kReplaceRootStage]) {
                const stages = getAggPlanStages(explain, stageName);
                assert.gt(stages.length, 0, {stageName, explain});
                for (const stage of stages) {
                    assert(stage.hasOwnProperty("nReturned"), {stageName, stage});
                    assert(stage.hasOwnProperty("executionTimeMillisEstimate"), {stageName, stage});
                }
            }
        });
    });

    describe("metadata retained when $$SEARCH_META is referenced downstream", function () {
        it("[standalone] retains metadata spec", function () {
            if (isSharded) return;
            const explain = coll
                .explain("queryPlanner")
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                    {$project: {name: 1, meta: "$$SEARCH_META"}},
                ]);
            const drmSpecs = getDrmSpecs(explain);
            assert.eq(drmSpecs.length, 1, {explain});
            assert.docEq(kDrmSpecWithMeta, drmSpecs[0], {explain});
        });

        it("[sharded] $setVariableFromSubPipeline appears in merger", function () {
            if (!isSharded || nShards < 2) return;
            const explain = coll
                .explain("queryPlanner")
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                    {$project: {name: 1, meta: "$$SEARCH_META"}},
                ]);
            assert(explain.splitPipeline, {explain});
            assert(hasSetVarInMerger(explain), {explain});
            assert(hasMergeCursors(explain), {explain});
            const shardsDrm = getSplitPipelineStages(explain, kDrmStage)
                .filter((s) => s.part === "shardsPart")
                .map((s) => s.stage[kDrmStage]);
            assert.gt(shardsDrm.length, 0, {explain});
            for (const spec of shardsDrm) {
                assert.docEq(kDrmSpecWithMetaSharded, spec, {explain});
            }
        });

        it("[sharded] executionStats explain reports per-shard execution stats", function () {
            if (!isSharded || nShards < 2) return;
            const explain = coll
                .explain("executionStats")
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                    {$project: {name: 1, meta: "$$SEARCH_META"}},
                ]);
            assert(explain.shards, {explain});
            // Each shard runs DRM + $replaceRoot; both must surface per-stage execution metrics.
            for (const stageName of [kDrmStage, kReplaceRootStage]) {
                const stages = getAggPlanStages(explain, stageName);
                assert.gt(stages.length, 0, {stageName, explain});
                for (const stage of stages) {
                    assert(stage.hasOwnProperty("nReturned"), {stageName, stage});
                    assert(stage.hasOwnProperty("executionTimeMillisEstimate"), {stageName, stage});
                }
            }
        });
    });

    describe("metadata elided when no downstream stage references $$SEARCH_META", function () {
        it("[standalone] elides metadata spec", function () {
            if (isSharded) return;
            const explain = coll
                .explain("queryPlanner")
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                    {$project: {name: 1}},
                ]);
            const drmSpecs = getDrmSpecs(explain);
            assert.eq(drmSpecs.length, 1, {explain});
            assert.docEq(kDrmSpecMetaElided, drmSpecs[0], {explain});
        });

        it("[sharded] no setVar in merger and shard-side DRM spec reflects elision", function () {
            if (!isSharded || nShards < 2) return;
            const explain = coll
                .explain("queryPlanner")
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                    {$project: {name: 1}},
                ]);
            assert(explain.splitPipeline, {explain});
            assert(!hasSetVarInMerger(explain), {explain});
            assert(hasMergeCursors(explain), {explain});
            const drmSpecs = getSplitPipelineStages(explain, kDrmStage)
                .filter((s) => s.part === "shardsPart")
                .map((s) => s.stage[kDrmStage]);
            assert.gt(drmSpecs.length, 0, {explain});
            for (const spec of drmSpecs) {
                assert.docEq(kDrmSpecMetaElided, spec, {explain});
            }
        });
    });

    describe("nested $unionWith surfaces the DRM stage in the subpipeline explain", function () {
        function runUnionWithExplain() {
            return coll.explain("queryPlanner").aggregate([
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta}},
                            {$project: {name: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ]);
        }

        it("[standalone] subpipeline contains DRM with metadata", function () {
            if (isSharded) return;
            const explain = runUnionWithExplain();
            const unionWithStage = getUnionWithStage(explain);
            assert(unionWithStage, {explain});
            const subPipeline = unionWithStage["$unionWith"].pipeline;
            assert(Array.isArray(subPipeline), {explain});
            const drmSpecs = subPipeline
                .filter((stage) => stage.hasOwnProperty(kDrmStage))
                .map((stage) => stage[kDrmStage]);
            assert.eq(drmSpecs.length, 1, {explain});
            assert.docEq(kDrmSpecWithMeta, drmSpecs[0], {explain});
        });

        it("[sharded] subpipeline contains DRM with metadata across shards", function () {
            if (!isSharded || nShards < 2) return;
            const explain = runUnionWithExplain();
            const unionWithStage = getUnionWithStage(explain);
            assert(unionWithStage, {explain});
            const subPipeline = unionWithStage["$unionWith"].pipeline;
            const shardStages = Object.values(subPipeline?.shards ?? {})
                .filter((s) => Array.isArray(s.stages))
                .flatMap((s) => s.stages);
            const drmSpecs = shardStages
                .filter((stage) => stage.hasOwnProperty(kDrmStage))
                .map((stage) => stage[kDrmStage]);
            assert.gt(drmSpecs.length, 0, {explain});
            for (const spec of drmSpecs) {
                assert.docEq(kDrmSpecWithMetaSharded, spec, {explain});
            }
        });
    });
});
