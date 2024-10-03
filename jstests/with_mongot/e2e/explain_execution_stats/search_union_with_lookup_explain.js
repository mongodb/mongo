/**
 * Tests explain for $search with a subpipeline ($unionWith and $lookup).
 * @tags: [
 * featureFlagSearchExplainExecutionStats,
 * ]
 */

import {getAggPlanStages, getUnionWithStage} from "jstests/libs/query/analyze_plan.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {
    verifyE2ESearchExplainOutput,
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numFireDocs = 10000;
let docs = [];
for (let i = 0; i < numFireDocs; i++) {
    docs.push({_id: i, a: i % 1000, element: "fire"});
}
assert.commandWorked(coll.insertMany(docs));

coll.createSearchIndex({name: "search-index", definition: {"mappings": {"dynamic": true}}});
const fireSearchQuery = {
    $search: {
        index: "search-index",
        text: {query: "fire", path: ["element"]},
    }
};

// Another collection for $lookup and $unionWith queries.
const collBase = db.base;
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

function runExplainTest(verbosity) {
    // Test with $lookup. $lookup does not include explain info about its subpipeline, so we check
    // the result of the $lookup output instead. We only check the value of "nReturned" when
    // executing with non "queryplanner" verbosity. Otherwise, we run the query to confirm the query
    // does not error.
    let result = collBase.explain(verbosity).aggregate([
        {$project: {"_id": 0}},
        {
            $lookup: {
                from: coll.getName(),
                pipeline: [
                    fireSearchQuery,
                    {
                        $project: {
                            "_id": 0,
                            "ref_id": "$_id",
                            "searchMeta": "$$SEARCH_META",
                        }
                    }],
                as: "fire_users"
            }
        }
    ]);
    if (verbosity != "queryPlanner") {
        let lookupStages = getAggPlanStages(result, "$lookup");
        let lookupReturned = 0;
        // In the sharded scenario, there will be more than one $lookup stage.
        for (let stage of lookupStages) {
            assert.neq(stage, null, result);
            assert(stage.hasOwnProperty("nReturned"));
            lookupReturned += stage["nReturned"];
        }
        assert.eq(NumberLong(2), lookupReturned);
    }

    // Test with $unionWith.
    result = collBase.explain(verbosity).aggregate([{
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                fireSearchQuery,
                {
                    $project: {
                        "_id": 0,
                        "ref_id": "$_id",
                        "searchMeta": "$$SEARCH_META",
                    }
                }
            ]
        }
    }]);

    let unionWithStage = getUnionWithStage(result);
    let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
    verifyE2ESearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(numFireDocs)
    });
    verifyE2ESearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(numFireDocs)
    });
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
