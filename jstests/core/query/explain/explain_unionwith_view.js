/**
 * Tests that explain executionStats on a $unionWith against a view properly resolves the view and
 * shows the view's pipeline in the explain output.
 * @tags: [
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *
 *   # For simplicity of explain analysis, this test does not run against sharded collections. For
 *   # the sharded cases, see jstests/noPassthrough/query/explain/explain_unionwith_sharded_view.js.
 *   assumes_unsharded_collection,
 *   assumes_standalone_mongod,
 *
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUnionWithStage} from "jstests/libs/query/analyze_plan.js";

const collName = "explain_unionwith_coll";
const coll = db[collName];
assertDropCollection(db, collName);

// Insert two documents. The view defined below will only contain the first one.
coll.insert({_id: 0, x: 1, y: 1});
coll.insert({_id: 1, x: 2, y: 2});

const viewName = "explain_unionwith_view";
assertDropCollection(db, viewName);
assert.commandWorked(
    db.createView(viewName, coll.getName(), [{$match: {x: 1, y: 1}}, {$project: {x: 1, y: 1}}]));

function checkExplainProperties(explainRoot, hasExecutionStats) {
    const unionWithPipelineExplain =
        getUnionWithStage(explainRoot, "$unionWith").$unionWith.pipeline[0].$cursor;

    // Since we have resolved the view in the pipeline we should see the view's $match stage in the
    // 'parsedQuery' of the $cursor stage.
    assert.eq({$and: [{x: {$eq: 1}}, {y: {$eq: 1}}]},
              unionWithPipelineExplain.queryPlanner.parsedQuery,
              "Did not find the expected parsedQuery for the $unionWith pipeline, the explain is " +
                  tojson(explainRoot));

    if (hasExecutionStats) {
        // Since the view only contains one document, we should see an 'nReturned' value of 1 in the
        // execution stats for the $unionWith pipeline.
        assert.eq(
            1,
            unionWithPipelineExplain.executionStats.nReturned,
            "Did not find the expected nReturned value for the $unionWith pipeline, the explain is " +
                tojson(explainRoot));
    }
};

const pipeline = [{$project: {y: 1}}];

// The default verbosity for explain is 'queryPlanner'.
jsTestLog("Running explain('queryPlanner') on collection:");
checkExplainProperties(
    coll.explain().aggregate([{$unionWith: {coll: viewName, pipeline: pipeline}}]));

jsTestLog("Running explain('executionStats') on collection:");
checkExplainProperties(
    coll.explain("executionStats").aggregate([{$unionWith: {coll: viewName, pipeline: pipeline}}]),
    true);

jsTestLog("Running explain('allPlansExecution') on collection:");
checkExplainProperties(coll.explain("allPlansExecution").aggregate([
    {$unionWith: {coll: viewName, pipeline: pipeline}}
]),
                       true);

jsTestLog("Running explain('queryPlanner') on view:");
checkExplainProperties(
    db[viewName].explain().aggregate([{$unionWith: {coll: viewName, pipeline: pipeline}}]));

jsTestLog("Running explain('executionStats') on view:");
checkExplainProperties(db[viewName].explain("executionStats").aggregate([
    {$unionWith: {coll: viewName, pipeline: pipeline}}
]),
                       true);

jsTestLog("Running explain('allPlansExecution') on view:");
checkExplainProperties(db[viewName].explain("allPlansExecution").aggregate([
    {$unionWith: {coll: viewName, pipeline: pipeline}}
]),
                       true);
