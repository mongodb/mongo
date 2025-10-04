/**
 * Tests that $rankFusion and $scoreFusion on a nested view namespace is allowed and works
 * correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {verifyExplainStagesAreEqual} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {createHybridSearchPipeline} from "jstests/with_mongot/e2e_lib/hybrid_search_on_view.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const nDocs = 50;
let bulk = coll.initializeOrderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    if (i % 2 === 0) {
        bulk.insert({_id: i, a: "foo", x: i / 3, y: i - 100, loc: [i, i]});
    } else {
        bulk.insert({_id: i, a: "bar", x: i / 2, y: i + 100, loc: [-i, -i]});
    }
}
assert.commandWorked(bulk.execute());

/**
 * This function creates a $rankFusion pipeline with the provided input pipelines. If a viewPipeline
 * is provided, it prepends the viewPipeline to each input pipeline.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to be prepended to the input pipelines.
 *                              If not provided, the input pipelines are used as is.
 */
export function createRankFusionPipeline(inputPipelines, viewPipeline = null) {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};

    return createHybridSearchPipeline(inputPipelines, viewPipeline, rankFusionStage);
}

/**
 * This function creates a $scoreFusion pipeline with the provided input pipelines. If a
 * viewPipeline is provided, it prepends the viewPipeline to each input pipeline.
 *
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to be prepended to the input pipelines.
 *                              If not provided, the input pipelines are used as is.
 */
export function createScoreFusionPipeline(inputPipelines, viewPipeline = null) {
    const scoreFusionStage = {
        $scoreFusion: {input: {pipelines: {}, normalization: "sigmoid"}, combination: {method: "avg"}},
    };

    return createHybridSearchPipeline(inputPipelines, viewPipeline, scoreFusionStage, /**isRankFusion*/ false);
}

// Define and create nested views.
const viewA = [{$match: {a: "foo"}}];
const viewB = [{$match: {x: {$gt: 5}}}];

assert.commandWorked(db.createView("viewA", coll.getName(), viewA));
assert.commandWorked(db.createView("viewB", "viewA", viewB));

const view = db["viewB"];

const runAggregationsOnNestedView = (pipelineWithViewPrepended, pipelineWithoutView) => {
    // Running a $rankFusion or $scoreFusion over the main collection with the view stage prepended
    // succeeds.
    const expectedResults = coll.aggregate(pipelineWithViewPrepended);
    const expectedExplain = coll.explain().aggregate(pipelineWithViewPrepended);

    // Running a $rankFusion or $scoreFusion against the view and removing the stage succeeds too.
    const viewResults = view.aggregate(pipelineWithoutView);
    const viewExplain = view.explain().aggregate(pipelineWithoutView);

    // Verify that the explain stages are the same.
    verifyExplainStagesAreEqual(viewExplain, expectedExplain);

    // Verify that the results are the same.
    assertDocArrExpectedFuzzy(expectedResults.toArray(), viewResults.toArray());
};

/* --------------------------------------------------------------------------------------- */
/* $rankFusion nested view test */

const rankFusionInputPipelines = {
    // Note, you have to place a $sort before a $limit to get deterministic results.
    a: [{$sort: {x: -1}}, {$limit: 10}],
    b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
};

const rankFusionPipelineWithViewPrepended = createRankFusionPipeline(rankFusionInputPipelines, [...viewA, ...viewB]);

const rankFusionPipelineWithoutView = createRankFusionPipeline(rankFusionInputPipelines);

runAggregationsOnNestedView(rankFusionPipelineWithViewPrepended, rankFusionPipelineWithoutView);

/* --------------------------------------------------------------------------------------- */
/* $scoreFusion nested view test */

const scoreFusionInputPipelines = {
    // Note, you have to place a $sort before a $limit to get deterministic results.
    a: [{$score: {score: "$x", normalization: "none"}}, {$sort: {x: 1}}, {$limit: 10}],
    b: [{$match: {x: {$lte: 15}}}, {$score: {score: "$y", normalization: "minMaxScaler"}}],
};

const scoreFusionPipelineWithViewPrepended = createScoreFusionPipeline(scoreFusionInputPipelines, [...viewA, ...viewB]);

const scoreFusionPipelineWithoutView = createScoreFusionPipeline(scoreFusionInputPipelines);

runAggregationsOnNestedView(scoreFusionPipelineWithViewPrepended, scoreFusionPipelineWithoutView);
