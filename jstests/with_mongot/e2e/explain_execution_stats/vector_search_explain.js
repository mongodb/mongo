/**
 * Test the use of "explain" with the "$vectorSearch" aggregation stage.
 * @tags: [
 * requires_fcv_81,
 * ]
 */
import {verifyE2EVectorSearchExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createGenericVectorSearchIndex,
    getGenericVectorSearchQuery,
} from "jstests/with_mongot/e2e_lib/generic_vector_search_utils.js";

const coll = createGenericVectorSearchIndex();

const highLimit = 1000;
const lowLimit = 5;

function runExplainTest(verbosity) {
    let result = coll.explain(verbosity).aggregate([getGenericVectorSearchQuery(lowLimit)]);
    verifyE2EVectorSearchExplainOutput({
        explainOutput: result,
        stageType: "$vectorSearch",
        verbosity,
        limit: lowLimit,
    });
    verifyE2EVectorSearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        limit: lowLimit,
    });

    result = coll.explain(verbosity).aggregate([getGenericVectorSearchQuery(highLimit)]);
    verifyE2EVectorSearchExplainOutput({
        explainOutput: result,
        stageType: "$vectorSearch",
        verbosity,
        limit: highLimit,
    });
    verifyE2EVectorSearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchIdLookup",
        limit: highLimit,
        verbosity,
    });
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
