/**
 * Tests explain for $vectorSearch in a $unionWith subpipeline.
 * @tags: [
 * requires_fcv_81,
 * ]
 */

import {getUnionWithStage} from "jstests/libs/query/analyze_plan.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {
    generateRandomVectorEmbedding,
    verifyE2EVectorSearchExplainOutput
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
let numDocs = 10000;

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
coll.createSearchIndex(index);

// Another collection for $unionWith.
const collBase = db.base;
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

const limit = 1000;
let vectorSearchQuery = {
    $vectorSearch: {
        queryVector: generateRandomVectorEmbedding(1000),
        path: "embedding",
        numCandidates: limit * 2,
        index: "vector_search",
        limit: limit,
    }
};

function runExplainTest(verbosity) {
    let result = collBase.explain(verbosity).aggregate([{
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                vectorSearchQuery,
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
