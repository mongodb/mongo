/**
 * Tests explain for $search.
 * @tags: [
 * requires_fcv_81,
 * ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {verifyE2ESearchExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numFireDocs = 10000;
let docs = [];
for (let i = 0; i < numFireDocs; i++) {
    docs.push({_id: i, a: i % 1000, element: "fire"});
}
const numWaterDocs = 15;
for (let i = numFireDocs; i < numFireDocs + numWaterDocs; i++) {
    docs.push({_id: i, a: i % 1000, element: "water"});
}
assert.commandWorked(coll.insertMany(docs));

createSearchIndex(coll, {name: "search-index", definition: {"mappings": {"dynamic": true}}});
const fireSearchQuery = {
    $search: {
        index: "search-index",
        text: {query: "fire", path: ["element"]},
    },
};

const waterSearchQuery = {
    $search: {
        index: "search-index",
        text: {query: "water", path: ["element"]},
    },
};

function runExplainTest(verbosity) {
    // Since there are only 15 water documents, no getMore is issued.
    let result = coll.explain(verbosity).aggregate([waterSearchQuery]);
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(numWaterDocs),
    });
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(numWaterDocs),
    });

    // There are 10,000 fire docs, so getMore's are issued.
    result = coll.explain(verbosity).aggregate([fireSearchQuery]);
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });

    // Test with $$SEARCH_META variable.
    result = coll.explain(verbosity).aggregate([
        fireSearchQuery,
        {
            $project: {
                "_id": 0,
                "ref_id": "$_id",
                "searchMeta": "$$SEARCH_META",
            },
        },
    ]);
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchMongotRemote",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });
    verifyE2ESearchExplainOutput({
        explainOutput: result,
        stageType: "$_internalSearchIdLookup",
        verbosity,
        nReturned: NumberLong(numFireDocs),
    });
    // On a sharded cluster, $setVariableFromSubPipeline should be inserted.
    if (result.hasOwnProperty("splitPipeline") && result["splitPipeline"] !== null) {
        let mergingPipeline = result.splitPipeline.mergerPart;
        assert.eq(["$mergeCursors"], Object.keys(mergingPipeline[0]));
        assert.eq(["$setVariableFromSubPipeline"], Object.keys(mergingPipeline[1]));
    }
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");

dropSearchIndex(coll, {name: "search-index"});
