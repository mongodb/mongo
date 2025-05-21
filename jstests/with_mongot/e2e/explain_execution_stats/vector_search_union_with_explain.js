/**
 * Tests explain for $vectorSearch in a $unionWith subpipeline.
 * @tags: [
 * requires_fcv_81,
 * ]
 */

import {getUnionWithStage} from "jstests/libs/query/analyze_plan.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {verifyE2EVectorSearchExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createGenericVectorSearchIndex,
    getGenericVectorSearchQuery,
} from "jstests/with_mongot/e2e_lib/generic_vector_search_utils.js";

const coll = createGenericVectorSearchIndex();

// Another collection for $unionWith.
const collBase = coll.getDB().getCollection("base");
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

const limit = 1000;
function runExplainTest(verbosity) {
    let result = collBase.explain(verbosity).aggregate([{
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                getGenericVectorSearchQuery(limit),
            ]
        }
    }]);

    let unionWithStage = getUnionWithStage(result);
    let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
    verifyE2EVectorSearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$vectorSearch",
        verbosity,
        limit: limit,
    });

    verifyE2EVectorSearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$_internalSearchIdLookup",
        limit: limit,
        verbosity,
    });
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
