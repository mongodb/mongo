
/**
 * Tests that $rankFusion on a view namespace is allowed and works correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

import {verifyExplainStagesAreEqual} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    assertDocArrExpectedFuzzy,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const nDocs = 50;
let bulk = coll.initializeOrderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    if (i % 2 === 0) {
        bulk.insert({_id: i, a: "foo", x: i / 3, loc: [i, i]});
    } else {
        bulk.insert({_id: i, a: "bar", x: i / 2, loc: [-i, -i]});
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
const createRankFusionPipeline = (inputPipelines, viewPipeline = null) => {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};

    for (const [key, pipeline] of Object.entries(inputPipelines)) {
        if (viewPipeline) {
            // If a viewPipeline is provided, prepend it to the input pipeline.
            rankFusionStage.$rankFusion.input.pipelines[key] = [...viewPipeline, ...pipeline];
        } else {
            // Otherwise, just use the input pipeline as is.
            rankFusionStage.$rankFusion.input.pipelines[key] = pipeline;
        }
    }

    return [rankFusionStage];
};

const viewA = [{$match: {a: "foo"}}];
const viewB = [{$match: {x: {$gt: 5}}}];

const rankFusionPipelineWithViewPrepended = createRankFusionPipeline({
    a: [{$sort: {x: -1}}, {$limit: 10}],
    b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
},
                                                                     [...viewA, ...viewB]);
const rankFusionPipelineWithoutView = createRankFusionPipeline({
    a: [{$sort: {x: -1}}, {$limit: 10}],
    b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
});

assert.commandWorked(db.createView("viewA", coll.getName(), viewA));
assert.commandWorked(db.createView("viewB", "viewA", viewB));

const view = db["viewB"];

// Running a $rankFusion over the main collection with the view stage prepended succeeds.
const expectedResults = coll.aggregate(rankFusionPipelineWithViewPrepended);
const expectedExplain = coll.explain().aggregate(rankFusionPipelineWithViewPrepended);

// Running a $rankFusion against the view and removing the stage succeeds too.
const viewResults = view.aggregate(rankFusionPipelineWithoutView);
const viewExplain = view.explain().aggregate(rankFusionPipelineWithoutView);

// Verify that the explain stages are the same.
verifyExplainStagesAreEqual(viewExplain, expectedExplain);

// Verify that the results are the same.
assertDocArrExpectedFuzzy(expectedResults.toArray(), viewResults.toArray());
