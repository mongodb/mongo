/**
 * Tests explain for $search with a subpipeline ($unionWith and $lookup).
 * @tags: [
 * requires_fcv_81,
 * ]
 */

import {getUnionWithStage} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {
    verifyE2ELookupSearchExplainOutput,
    verifyE2ESearchExplainOutput,
} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numFireDocs = 10000;
let docs = [];
for (let i = 0; i < numFireDocs; i++) {
    docs.push({_id: i, a: i % 1000, element: "fire"});
}
assert.commandWorked(coll.insertMany(docs));

createSearchIndex(coll, {name: "search-index", definition: {"mappings": {"dynamic": true}}});
const fireSearchQuery = {
    $search: {
        index: "search-index",
        text: {query: "fire", path: ["element"]},
    },
};

// Another collection for $lookup and $unionWith queries.
const collBase = db.base;
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

function runExplainTest(verbosity) {
    // Test with $lookup. $lookup serializes the resolved and optimized sub-pipeline in explain
    // output. Verify the sub-pipeline is present and check nReturned for non-queryPlanner
    // verbosities.
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
                        },
                    },
                ],
                as: "fire_users",
            },
        },
    ]);
    verifyE2ELookupSearchExplainOutput({
        explainOutput: result,
        searchStageType: "$search",
        verbosity,
        nReturned: NumberLong(2),
    });

    // Test with $unionWith.
    result = collBase.explain(verbosity).aggregate([
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [
                    fireSearchQuery,
                    {
                        $project: {
                            "_id": 0,
                            "ref_id": "$_id",
                            "searchMeta": "$$SEARCH_META",
                        },
                    },
                ],
            },
        },
    ]);

    let unionWithStage = getUnionWithStage(result);
    let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
    verifyE2ESearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });
    verifyE2ESearchExplainOutput({
        explainOutput: unionSubExplain,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
dropSearchIndex(coll, {name: "search-index"});
