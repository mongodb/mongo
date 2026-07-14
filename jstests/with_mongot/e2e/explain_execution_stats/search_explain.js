/**
 * Tests explain for $search with real mongot.
 * E2E version of jstests/with_mongot/search_mocked/search_explain.js,
 * search_explain_execution_stats.js, and search_meta_var_explain.js.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {verifyE2ESearchExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const coll = db[jsTestName()];
const indexName = jsTestName() + "_index";

const numFireDocs = 10000;
const numWaterDocs = 15;

describe("$search explain", function () {
    before(function () {
        coll.drop();
        let docs = [];
        for (let i = 0; i < numFireDocs; i++) {
            docs.push({_id: i, a: i % 1000, element: "fire"});
        }
        for (let i = numFireDocs; i < numFireDocs + numWaterDocs; i++) {
            docs.push({_id: i, a: i % 1000, element: "water"});
        }
        assert.commandWorked(coll.insertMany(docs));

        createSearchIndex(coll, {name: indexName, definition: {"mappings": {"dynamic": true}}});
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    const fireSearchQuery = {
        $search: {
            index: indexName,
            text: {query: "fire", path: ["element"]},
        },
    };

    const waterSearchQuery = {
        $search: {
            index: indexName,
            text: {query: "water", path: ["element"]},
        },
    };

    function verifySearchStages(result, verbosity, nReturned) {
        verifyE2ESearchExplainOutput({
            explainOutput: result,
            stageType: "$_internalSearchMongotRemote",
            verbosity,
            nReturned: NumberLong(nReturned),
        });
        verifyE2ESearchExplainOutput({
            explainOutput: result,
            stageType: "$_internalSearchIdLookup",
            verbosity,
            nReturned: NumberLong(nReturned),
        });
    }

    for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
        describe(verbosity, function () {
            it("validates explain for no-getMore workload", function () {
                const result = coll.explain(verbosity).aggregate([waterSearchQuery]);
                verifySearchStages(result, verbosity, numWaterDocs);
            });

            it("validates explain for getMore workload", function () {
                const result = coll.explain(verbosity).aggregate([fireSearchQuery]);
                verifySearchStages(result, verbosity, numFireDocs);
            });

            it("validates explain with $$SEARCH_META projection", function () {
                const fireSearchWithCount = {
                    $search: {
                        index: indexName,
                        text: {query: "fire", path: ["element"]},
                        count: {type: "total"},
                    },
                };
                const result = coll.explain(verbosity).aggregate([
                    fireSearchWithCount,
                    {
                        $project: {
                            "_id": 0,
                            "ref_id": "$_id",
                            "searchMeta": "$$SEARCH_META",
                        },
                    },
                ]);
                verifySearchStages(result, verbosity, numFireDocs);
                if (result.hasOwnProperty("splitPipeline") && result["splitPipeline"] !== null) {
                    let mergingPipeline = result.splitPipeline.mergerPart;
                    assert.eq(["$mergeCursors"], Object.keys(mergingPipeline[0]));
                    assert.eq(["$setVariableFromSubPipeline"], Object.keys(mergingPipeline[1]));
                }
            });
        });
    }
});
