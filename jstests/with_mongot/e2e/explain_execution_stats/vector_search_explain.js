/**
 * Test the use of "explain" with the "$vectorSearch" aggregation stage.
 * @tags: [
 * requires_fcv_81,
 * ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    generateRandomVectorEmbedding,
    verifyE2EVectorSearchExplainOutput,
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numDocs = 10000;

let docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({_id: i, a: i % 1000, embedding: generateRandomVectorEmbedding(1000)});
}
assert.commandWorked(coll.insertMany(docs));
let index = {
    name: "vector_search",
    type: "vectorSearch",
    definition: {
        "fields": [{
            "type": "vector",
            "numDimensions": 1000,
            "path": "embedding",
            "similarity": "euclidean"
        }]
    }
};
createSearchIndex(coll, index);

const highLimit = 1000;
const lowLimit = 5;

function getQuery(limit) {
    return {
        $vectorSearch: {
            queryVector: generateRandomVectorEmbedding(1000),
            path: "embedding",
            numCandidates: limit * 2,
            index: "vector_search",
            limit: limit,
        }
    };
}

function runExplainTest(verbosity) {
    let result = coll.explain(verbosity).aggregate([getQuery(lowLimit)]);
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

    result = coll.explain(verbosity).aggregate([getQuery(highLimit)]);
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
dropSearchIndex(coll, {name: "vector_search"});
