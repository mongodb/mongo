/**
 * Tests extension optimization rules (applyPipelineBounds, applyMatchPushdown,
 * REDUNDANT_SORT_REMOVAL, eraseStage) in two scenarios:
 *
 * 1. Extension INSIDE $rankFusion/$scoreFusion input pipelines (sections 1-2): uses
 *    $testSelectionOptimization, a selection stage (isSelectionStage:true) with applyPipelineBounds
 *    and applyMatchPushdown hooks. Each input pipeline is prefixed with {$sort} to satisfy
 *    $rankFusion's requirement that every input pipeline begin with a ranked stage or $sort (code
 *    12108702). $testVectorSearchOptimization is not usable here because it is a non-selection
 *    stage rejected by the RF/SF input pipeline validator (code 12108704) on all topologies.
 *
 * 2. Extension AFTER $rankFusion/$scoreFusion in the outer pipeline (sections 3-4): uses
 *    $testVectorSearchOptimization with desugar:false. Testing extensions BEFORE hybrid search
 *    stages is not possible because $rankFusion/$scoreFusion must be the first stage of an
 *    aggregation pipeline (code 10170100) on all topologies.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   requires_fcv_90,
 * ]
 */
import {before, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {getAggStagesAcrossSplitPipeline} from "jstests/libs/query/analyze_plan.js";

// desugar:false → single $testVectorSearch stage; clean explain for optimization checks.
const desugarFalseStage = {$testVectorSearchOptimization: {desugar: false}};
const sortVS = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}};
// $testSelectionOptimization is a selection stage (isSelectionStage:true) with applyPipelineBounds
// and applyMatchPushdown hooks, so it passes the RF/SF input pipeline validator.
const selectionOptStage = {$testSelectionOptimization: {}};

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb[jsTestName()];
const outerColl = testDb[jsTestName() + "_outer"];

let createdViews = [];
let viewCounter = 0;

function makeView(suffix, source, pipeline) {
    const name = jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
    assert.commandWorked(testDb.createView(name, source, pipeline));
    createdViews.push(name);
    return name;
}

function dropCreatedViews() {
    for (let i = createdViews.length - 1; i >= 0; i--) {
        assert(testDb[createdViews[i]].drop());
    }
    createdViews = [];
}

function setupCollections() {
    assert.commandWorked(testDb.dropDatabase());
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(outerColl.insertMany([{_id: 0}]));
}

function explainAgg(ns, pipeline) {
    return testDb[ns].explain("queryPlanner").aggregate(pipeline);
}

function stageCount(explain, stageName) {
    return getAggStagesAcrossSplitPipeline(explain, stageName).length;
}

// Reads $testVectorSearch bounds from the first occurrence in the main pipeline explain.
function getTestVectorSearchBounds(explain) {
    const stages = getAggStagesAcrossSplitPipeline(explain, "$testVectorSearch");
    const limit = stages.length > 0 ? stages[0].$testVectorSearch?.limit : undefined;
    return {
        minBoundsType: limit?.minBoundsType,
        maxBoundsType: limit?.maxBoundsType,
        pipelineBoundsLimit: limit?.pipelineBoundsLimit,
    };
}

// Returns the inner spec of the first $testSelectionOptimization stage in the explain.
function getSelectionOptSpec(explain) {
    const stages = getAggStagesAcrossSplitPipeline(explain, "$testSelectionOptimization");
    return stages.length > 0 ? stages[0].$testSelectionOptimization : undefined;
}

// Asserts applyPipelineBounds ran and recorded discrete bounds with the given limit.
function assertSelectionOptDiscreteBounds(explain, limit) {
    const spec = getSelectionOptSpec(explain);
    assert(spec !== undefined, "expected $testSelectionOptimization in explain", {spec});
    assert.eq(spec.minBoundsType, "discrete", "wrong minBoundsType", {spec});
    assert.eq(spec.maxBoundsType, "discrete", "wrong maxBoundsType", {spec});
    assert.eq(spec.extractedLimit, limit, "wrong extractedLimit", {spec});
}

// Asserts applyMatchPushdown ran and raised startId to the expected value.
function assertSelectionOptStartId(explain, startId) {
    const spec = getSelectionOptSpec(explain);
    assert(spec !== undefined, "expected $testSelectionOptimization in explain", {spec});
    assert.eq(spec.startId, startId, "expected startId from applyMatchPushdown", {spec});
}

// Asserts applyPipelineBounds ran on $testVectorSearch and recorded discrete bounds with the given
// limit.
function assertVSBoundsDiscrete(explain, limit) {
    const {minBoundsType, maxBoundsType, pipelineBoundsLimit} = getTestVectorSearchBounds(explain);
    assert.eq(minBoundsType, "discrete", "wrong minBoundsType", {minBoundsType});
    assert.eq(maxBoundsType, "discrete", "wrong maxBoundsType", {maxBoundsType});
    assert.eq(pipelineBoundsLimit, limit, "wrong pipelineBoundsLimit", {pipelineBoundsLimit});
}

// Returns true if an explain $sort stage sorts by vectorSearchScore metadata.
function isSortByVectorSearchScore(stage) {
    const key = stage.$sort && stage.$sort.sortKey;
    return key && key.vectorSearchScore && key.vectorSearchScore.$meta === "vectorSearchScore";
}

// Asserts REDUNDANT_SORT_REMOVAL erased the vectorSearchScore sort.
// Checks only $sort stages with the specific vectorSearchScore key, ignoring any $rankFusion-
// internal sorts, so unrelated planning changes cannot mask a failure.
function assertVSSortErased(ns, pipeline) {
    const stages = getAggStagesAcrossSplitPipeline(explainAgg(ns, pipeline), "$sort");
    const count = stages.filter(isSortByVectorSearchScore).length;
    assert.eq(count, 0, "REDUNDANT_SORT_REMOVAL should have erased the vectorSearchScore sort", {
        count,
    });
}

// Confirms eraseStage erased `stageName` by asserting adding it doesn't change the plan count.
function assertOptimizationErasedStage(ns, baselinePipeline, pipelineWithExtra, stageName) {
    const baseline = stageCount(explainAgg(ns, baselinePipeline), stageName);
    const withExtra = stageCount(explainAgg(ns, pipelineWithExtra), stageName);
    assert.eq(withExtra, baseline, "optimization should have erased the extra stage", {
        stageName,
        baseline,
        withExtra,
    });
}

const rfNoExt = {
    $rankFusion: {input: {pipelines: {a: [{$sort: {x: 1}}], b: [{$sort: {y: 1}}]}}},
};

function makeRankFusion(pipelineA) {
    return {$rankFusion: {input: {pipelines: {a: pipelineA, b: [{$sort: {y: 1}}]}}}};
}

const scoreStageX = {$score: {score: "$x", normalization: "minMaxScaler"}};
const scoreStageY = {$score: {score: "$y", normalization: "minMaxScaler"}};

function makeScoreFusion(pipelineA) {
    return {
        $scoreFusion: {
            input: {pipelines: {a: pipelineA, b: [scoreStageY]}, normalization: "none"},
            combination: {method: "avg"},
        },
    };
}

const docs = [
    {_id: 1, x: 1, y: 10},
    {_id: 2, x: 2, y: 20},
    {_id: 3, x: 3, y: 30},
    {_id: 4, x: 4, y: 40},
];

describe("extension inside $rankFusion input pipeline", function () {
    before(setupCollections);
    afterEach(dropCreatedViews);

    describe("applyPipelineBounds", function () {
        it("direct: extracts $limit from input pipeline A as discrete bounds", function () {
            assertSelectionOptDiscreteBounds(
                explainAgg(coll.getName(), [
                    makeRankFusion([{$sort: {x: 1}}, selectionOptStage, {$limit: 2}]),
                ]),
                2,
            );
        });

        it("on view: extracts $limit when $rankFusion is queried on a view", function () {
            const viewName = makeView("rf_sel_bounds", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertSelectionOptDiscreteBounds(
                explainAgg(viewName, [
                    makeRankFusion([{$sort: {x: 1}}, selectionOptStage, {$limit: 2}]),
                ]),
                2,
            );
        });

        it("in $lookup subpipeline: pipeline executes successfully", function () {
            const res = outerColl
                .aggregate([
                    {
                        $lookup: {
                            from: coll.getName(),
                            as: "ranked",
                            pipeline: [
                                makeRankFusion([{$sort: {x: 1}}, selectionOptStage, {$limit: 2}]),
                            ],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 1, "one outer doc expected", {res});
            assert.gt(res[0].ranked.length, 0, "expected non-empty results from $lookup", {res});
        });
    });

    describe("applyMatchPushdown", function () {
        it("direct: folds $match{_id:>=2} into startId, erases $match from input pipeline", function () {
            assertSelectionOptStartId(
                explainAgg(coll.getName(), [
                    makeRankFusion([
                        {$sort: {x: 1}},
                        selectionOptStage,
                        {$match: {_id: {$gte: 2}}},
                    ]),
                ]),
                2,
            );
        });

        it("on view: folds $match when $rankFusion is queried on a view", function () {
            const viewName = makeView("rf_sel_mpd", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertSelectionOptStartId(
                explainAgg(viewName, [
                    makeRankFusion([
                        {$sort: {x: 1}},
                        selectionOptStage,
                        {$match: {_id: {$gte: 2}}},
                    ]),
                ]),
                2,
            );
        });

        it("in $lookup subpipeline: pipeline executes successfully", function () {
            const res = outerColl
                .aggregate([
                    {
                        $lookup: {
                            from: coll.getName(),
                            as: "results",
                            pipeline: [
                                makeRankFusion([
                                    {$sort: {x: 1}},
                                    selectionOptStage,
                                    {$match: {_id: {$gte: 2}}},
                                ]),
                            ],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 1, "one outer doc expected", {res});
            assert.gt(res[0].results.length, 0, "expected non-empty results", {res});
        });
    });
});

describe("extension inside $scoreFusion input pipeline", function () {
    before(setupCollections);
    afterEach(dropCreatedViews);

    describe("applyPipelineBounds", function () {
        it("direct: extracts $limit from input pipeline A as discrete bounds", function () {
            assertSelectionOptDiscreteBounds(
                explainAgg(coll.getName(), [
                    makeScoreFusion([selectionOptStage, {$limit: 2}, scoreStageX]),
                ]),
                2,
            );
        });

        it("on view: extracts $limit when $scoreFusion is queried on a view", function () {
            const viewName = makeView("sf_sel_bounds", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertSelectionOptDiscreteBounds(
                explainAgg(viewName, [
                    makeScoreFusion([selectionOptStage, {$limit: 2}, scoreStageX]),
                ]),
                2,
            );
        });

        it("in $unionWith subpipeline: pipeline executes successfully", function () {
            const res = coll
                .aggregate([
                    {$match: {_id: {$lt: 0}}},
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [
                                makeScoreFusion([selectionOptStage, {$limit: 2}, scoreStageX]),
                            ],
                        },
                    },
                ])
                .toArray();
            assert.gt(res.length, 0, "expected non-empty results", {res});
        });
    });

    describe("applyMatchPushdown", function () {
        it("direct: folds $match{_id:>=2} into startId", function () {
            assertSelectionOptStartId(
                explainAgg(coll.getName(), [
                    makeScoreFusion([selectionOptStage, {$match: {_id: {$gte: 2}}}, scoreStageX]),
                ]),
                2,
            );
        });

        it("on view: folds $match when $scoreFusion is queried on a view", function () {
            const viewName = makeView("sf_sel_mpd", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertSelectionOptStartId(
                explainAgg(viewName, [
                    makeScoreFusion([selectionOptStage, {$match: {_id: {$gte: 2}}}, scoreStageX]),
                ]),
                2,
            );
        });

        it("in $unionWith subpipeline: pipeline executes successfully", function () {
            const res = coll
                .aggregate([
                    {$match: {_id: {$lt: 0}}},
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [
                                makeScoreFusion([
                                    selectionOptStage,
                                    {$match: {_id: {$gte: 2}}},
                                    scoreStageX,
                                ]),
                            ],
                        },
                    },
                ])
                .toArray();
            assert.gt(res.length, 0, "expected non-empty results", {res});
        });
    });
});

const sfNoExt = {
    $scoreFusion: {
        input: {
            pipelines: {a: [scoreStageX], b: [scoreStageY]},
            normalization: "none",
        },
        combination: {method: "avg"},
    },
};

describe("extension after $rankFusion in outer pipeline", function () {
    before(setupCollections);
    afterEach(dropCreatedViews);

    describe("REDUNDANT_SORT_REMOVAL", function () {
        it("direct: erases sort after extension following $rankFusion", function () {
            assertVSSortErased(coll.getName(), [rfNoExt, desugarFalseStage, sortVS]);
        });

        it("on view: fires when $rankFusion is queried on a view", function () {
            const viewName = makeView("ext_after_hs", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertVSSortErased(viewName, [rfNoExt, desugarFalseStage, sortVS]);
        });

        it("in $lookup subpipeline: [$rankFusion, ext, sortVS] succeeds", function () {
            const res = outerColl
                .aggregate([
                    {
                        $lookup: {
                            from: coll.getName(),
                            as: "results",
                            pipeline: [rfNoExt, desugarFalseStage, sortVS],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 1, "one outer doc expected", {res});
            assert.gt(res[0].results.length, 0, "results expected from $lookup", {res});
        });
    });

    describe("eraseStage", function () {
        it("direct: erases $project{_id:1} after extension following $rankFusion", function () {
            assertOptimizationErasedStage(
                coll.getName(),
                [rfNoExt, desugarFalseStage],
                [rfNoExt, desugarFalseStage, {$project: {_id: 1}}],
                "$project",
            );
        });
    });

    describe("applyPipelineBounds", function () {
        it("direct: extracts $limit after extension following $rankFusion", function () {
            assertVSBoundsDiscrete(
                explainAgg(coll.getName(), [rfNoExt, desugarFalseStage, {$limit: 2}]),
                2,
            );
        });

        it("on view: extracts $limit when $rankFusion is queried on a view", function () {
            const viewName = makeView("bounds_after_rf", coll.getName(), [
                {$match: {x: {$gte: 1}}},
            ]);
            assertVSBoundsDiscrete(
                explainAgg(viewName, [rfNoExt, desugarFalseStage, {$limit: 2}]),
                2,
            );
        });

        it("in $unionWith subpipeline: $limit is respected", function () {
            const res = coll
                .aggregate([
                    {$match: {_id: {$lt: 0}}},
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [rfNoExt, desugarFalseStage, {$limit: 2}],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 2, "exactly 2 docs expected after $limit:2", {res});
        });
    });
});

describe("extension after $scoreFusion in outer pipeline", function () {
    before(setupCollections);
    afterEach(dropCreatedViews);

    describe("REDUNDANT_SORT_REMOVAL", function () {
        it("direct: erases sort after extension following $scoreFusion", function () {
            assertVSSortErased(coll.getName(), [sfNoExt, desugarFalseStage, sortVS]);
        });

        it("on view: fires when $scoreFusion is queried on a view", function () {
            const viewName = makeView("ext_after_sf", coll.getName(), [{$match: {x: {$gte: 1}}}]);
            assertVSSortErased(viewName, [sfNoExt, desugarFalseStage, sortVS]);
        });

        it("in $lookup subpipeline: [$scoreFusion, ext, sortVS] succeeds", function () {
            const res = outerColl
                .aggregate([
                    {
                        $lookup: {
                            from: coll.getName(),
                            as: "results",
                            pipeline: [sfNoExt, desugarFalseStage, sortVS],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 1, "one outer doc expected", {res});
            assert.gt(res[0].results.length, 0, "results expected from $lookup", {res});
        });
    });

    describe("eraseStage", function () {
        it("direct: erases $project{_id:1} after extension following $scoreFusion", function () {
            assertOptimizationErasedStage(
                coll.getName(),
                [sfNoExt, desugarFalseStage],
                [sfNoExt, desugarFalseStage, {$project: {_id: 1}}],
                "$project",
            );
        });
    });

    describe("applyPipelineBounds", function () {
        it("direct: extracts $limit after extension following $scoreFusion", function () {
            assertVSBoundsDiscrete(
                explainAgg(coll.getName(), [sfNoExt, desugarFalseStage, {$limit: 2}]),
                2,
            );
        });

        it("on view: extracts $limit when $scoreFusion is queried on a view", function () {
            const viewName = makeView("bounds_after_sf", coll.getName(), [
                {$match: {x: {$gte: 1}}},
            ]);
            assertVSBoundsDiscrete(
                explainAgg(viewName, [sfNoExt, desugarFalseStage, {$limit: 2}]),
                2,
            );
        });

        it("in $unionWith subpipeline: $limit is respected", function () {
            const res = coll
                .aggregate([
                    {$match: {_id: {$lt: 0}}},
                    {
                        $unionWith: {
                            coll: coll.getName(),
                            pipeline: [sfNoExt, desugarFalseStage, {$limit: 2}],
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 2, "exactly 2 docs expected after $limit:2", {res});
        });
    });
});
