/**
 * Tests explain for $searchMeta with a subpipeline ($unionWith and $lookup).
 * @tags: [
 * featureFlagSearchExplainExecutionStats,
 * ]
 */

import {getAggPlanStages, getUnionWithStage} from "jstests/libs/analyze_plan.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {
    verifyE2ESearchMetaExplainOutput,
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numDocs = 10000;
let docs = [];

let genres = [
    "Drama",
    "Comedy",
    "Romance",
];
for (let i = 0; i < numDocs; i++) {
    const genre = genres[i % 10];
    docs.push({_id: i, index: i % 1000, genre: genre});
}
assert.commandWorked(coll.insertMany(docs));

coll.createSearchIndex({
    name: "facet-index",
    definition: {
        "mappings": {
            "dynamic": false,
            "fields": {"index": {"type": "number"}, "genre": {"type": "stringFacet"}}
        }
    }
});

const facetQuery = {
    "$searchMeta": {
        "index": "facet-index",
        "facet": {
            "operator": {"range": {"path": "index", "gte": 0, "lte": 1000}},
            "facets": {"genresFacet": {"type": "string", "path": "genre"}},
        },
    }
};

// Another collection for $lookup and $unionWith queries.
const collBase = db.base;
collBase.drop();
assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

function runExplainTest(verbosity) {
    let result = collBase.explain(verbosity).aggregate([{
        $unionWith: {
            coll: coll.getName(),
            pipeline: [
                facetQuery,
            ]
        }
    }]);

    let unionWithStage = getUnionWithStage(result);
    let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
    verifyE2ESearchMetaExplainOutput(
        {explainOutput: unionSubExplain, numFacetBucketsAndCount: 4, verbosity: verbosity});

    // Test with $lookup. $lookup does not include explain info about its subpipeline, so we
    // check the result of the $lookup output instead. We only check the value of "nReturned"
    // when executing with non "queryplanner" verbosity. Otherwise, we run the query to confirm
    // the query does not error.
    result = collBase.explain(verbosity).aggregate([
        {$project: {"_id": 0}},
        {$lookup: {from: coll.getName(), pipeline: [facetQuery], as: "meta_facet"}}
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
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
