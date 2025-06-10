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

/**
 * This function creates a view with the provided name and pipeline, runs a $rankFusion against the
 * view, then runs the same $rankFusion against the main collection with the view stage prepended to
 * the input pipelines in the same way that the view desugaring works. It compares both the results
 * and the explain output of both queries to ensure they are the same.
 *
 * @param {string} testName name of the test, used to create a unique view.
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to create a view and to be prepended manually to the input
 *                              pipelines.
 * @param {bool}   checkCorrectness some pipelines are able to parse and run, but the order of the
 *                                  stages or the stages themselves are non-deterministic, so we
 *                                  can't check for correctness.
 */
const runRankFusionViewTest = (testName, inputPipelines, viewPipeline, checkCorrectness) => {
    // Create a view with viewStage.
    const viewName = testName + "_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    // Create the rankFusion pipeline with the view stage manually prepended.
    const rankFusionPipelineWithViewPrepended =
        createRankFusionPipeline(inputPipelines, viewPipeline);

    // Create the rankFusion pipeline without the view stage
    const rankFusionPipelineWithoutView = createRankFusionPipeline(inputPipelines);

    // Running a $rankFusion over the main collection with the view stage prepended succeeds.
    const expectedResults = coll.aggregate(rankFusionPipelineWithViewPrepended);
    const expectedExplain = coll.explain().aggregate(rankFusionPipelineWithViewPrepended);

    // Running a $rankFusion against the view and removing the stage succeeds too.
    const viewResults = view.aggregate(rankFusionPipelineWithoutView);
    const viewExplain = view.explain().aggregate(rankFusionPipelineWithoutView);

    // Verify that the explain stages are the same.
    verifyExplainStagesAreEqual(viewExplain, expectedExplain);

    // Verify that the results are the same.
    if (checkCorrectness) {
        assertDocArrExpectedFuzzy(expectedResults.toArray(), viewResults.toArray());
    }
};

// TODO SERVER-105677: Add tests for $skip.
// TODO SERVER-105862: Add tests for $geoNear in the view definition.
// Excluded tests:
// - $geoNear can't run against views.
runRankFusionViewTest("simple_match",
                      {
                          a: [{$match: {x: {$gt: 5}}}, {$sort: {x: -1}}],
                          b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
                      },
                      [{$match: {a: "foo"}}],
                      /*checkCorrectness=**/ true);
runRankFusionViewTest("match_and_limit",
                      {
                          a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                          b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                      },
                      [{$match: {a: "bar"}}],
                      /*checkCorrectness=**/ true);
runRankFusionViewTest("limit_in_view",
                      {
                          a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
                          b: [{$match: {x: {$lt: 10}}}, {$sort: {x: 1}}],
                      },
                      [{$sort: {x: -1}}, {$limit: 15}],
                      /*checkCorrectness=**/ true);
runRankFusionViewTest("no_match",
                      {
                          a: [{$sort: {x: 1}}],
                          b: [{$sort: {x: -1}}],
                      },
                      [{$sort: {x: -1}}, {$limit: 15}],
                      /*checkCorrectness=**/ true);
runRankFusionViewTest("three_pipelines",
                      {
                          a: [{$match: {a: "foo"}}, {$sort: {x: 1}}],
                          b: [{$match: {a: "bar"}}, {$sort: {x: 1}}],
                          c: [{$match: {x: {$lt: 10}}}, {$sort: {x: -1}}],
                      },
                      [{$match: {x: {$gt: 2}}}],
                      /*checkCorrectness=**/ true);
runRankFusionViewTest("limit_in_input",
                      {
                          a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
                          b: [{$limit: 5}, {$sort: {x: 1}}],
                      },
                      [{$match: {x: {$lt: 20}}}],
                      /*checkCorrectness=**/ false);
runRankFusionViewTest("sample",
                      {
                          a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
                          b: [{$sample: {size: 5}}, {$sort: {x: 1}}],
                      },
                      [{$match: {x: {$lt: 20}}}],
                      /*checkCorrectness=**/ false);
