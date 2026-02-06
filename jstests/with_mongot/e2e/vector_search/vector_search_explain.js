/**
 * Test the use of "explain" with the "$vectorSearch" aggregation stage using real mongot.
 *
 * E2E version of with_mongot/vector_search_mocked/vector_search_explain.js
 *
 * Sharded-specific explain tests are in sharding_no_passthrough/vector_search_explain_sharded.js
 *
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {verifyE2EVectorSearchExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 10;
const limit = 5;
const index = "vector_search_explain_index";

describe("$vectorSearch explain", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, x: [1.0, 2.0, 3.0]},
                {_id: 1, x: [1.1, 2.1, 3.1]},
                {_id: 2, x: [0.9, 1.9, 2.9], color: "red"},
                {_id: 3, x: [0.8, 1.8, 2.8], color: "blue"},
                {_id: 4, x: [0.7, 1.7, 2.7], color: "red"},
                {_id: 5, x: [0.6, 1.6, 2.6]},
                {_id: 6, x: [0.5, 1.5, 2.5], color: "blue"},
                {_id: 7, x: [0.4, 1.4, 2.4]},
            ]),
        );

        createSearchIndex(coll, {
            name: index,
            type: "vectorSearch",
            definition: {
                fields: [
                    {
                        type: "vector",
                        path: path,
                        numDimensions: queryVector.length,
                        similarity: "cosine",
                    },
                    {
                        type: "filter",
                        path: "color",
                    },
                ],
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: index});
        coll.drop();
    });

    it("should return explain output for all verbosity levels", function () {
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit, index}}];

        for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
            const result = coll.explain(verbosity).aggregate(pipeline);

            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$vectorSearch",
                verbosity,
                limit,
            });
            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$_internalSearchIdLookup",
                verbosity,
                limit,
            });
        }
    });

    it("should return explain output with filter", function () {
        // Only 2 documents have color: "red", so use limit: 2 to ensure nReturned matches.
        const filterLimit = 2;
        const filter = {color: {$eq: "red"}};
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: filterLimit, index, filter}}];

        for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
            const result = coll.explain(verbosity).aggregate(pipeline);

            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$vectorSearch",
                verbosity,
                limit: filterLimit,
            });
            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$_internalSearchIdLookup",
                verbosity,
                limit: filterLimit,
            });
        }
    });

    it("should handle explain with small batchSize triggering getMore", function () {
        const pipeline = [
            {$vectorSearch: {queryVector, path, numCandidates, limit, index}},
            {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}},
        ];

        for (const verbosity of ["executionStats", "allPlansExecution"]) {
            const result = coll.explain(verbosity).aggregate(pipeline, {cursor: {batchSize: 2}});

            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$vectorSearch",
                verbosity,
                limit,
            });
            verifyE2EVectorSearchExplainOutput({
                explainOutput: result,
                stageType: "$_internalSearchIdLookup",
                verbosity,
                limit,
            });
        }
    });

    it("should return explain output with different limits", function () {
        for (const testLimit of [1, 3, 5]) {
            const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: testLimit, index}}];

            for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
                const result = coll.explain(verbosity).aggregate(pipeline);

                verifyE2EVectorSearchExplainOutput({
                    explainOutput: result,
                    stageType: "$vectorSearch",
                    verbosity,
                    limit: testLimit,
                });
                verifyE2EVectorSearchExplainOutput({
                    explainOutput: result,
                    stageType: "$_internalSearchIdLookup",
                    verbosity,
                    limit: testLimit,
                });
            }
        }
    });

    // Test $limit optimization with vectorSearch for both lower and higher user limits.
    // When userLimit < vectorSearchLimit, $limit is optimized to userLimit.
    // When userLimit > vectorSearchLimit, $limit stays at userLimit (no optimization needed).
    for (const {userLimit, description, expectOptimizedLimit} of [
        {userLimit: 3, description: "lower than", expectOptimizedLimit: true},
        {userLimit: 7, description: "higher than", expectOptimizedLimit: false},
    ]) {
        it(`should handle $limit ${description} $vectorSearch limit`, function () {
            const vectorSearchLimit = 5;
            const pipeline = [
                {$vectorSearch: {queryVector, path, numCandidates, limit: vectorSearchLimit, index}},
                {$limit: userLimit},
            ];

            // The effective limit for vectorSearch stages is min(userLimit, vectorSearchLimit).
            const effectiveLimit = Math.min(userLimit, vectorSearchLimit);

            for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
                const result = coll.explain(verbosity).aggregate(pipeline);

                verifyE2EVectorSearchExplainOutput({
                    explainOutput: result,
                    stageType: "$vectorSearch",
                    verbosity,
                    limit: effectiveLimit,
                });
                verifyE2EVectorSearchExplainOutput({
                    explainOutput: result,
                    stageType: "$_internalSearchIdLookup",
                    verbosity,
                    limit: effectiveLimit,
                });

                // Validate that $limit stages exist in the explain output.
                const limitStages = getAggPlanStages(result, "$limit");
                assert.gt(
                    limitStages.length,
                    0,
                    `Expected at least one $limit stage in explain output: ${tojson(result)}`,
                );

                // When userLimit < vectorSearchLimit, $limit is optimized to effectiveLimit.
                // When userLimit > vectorSearchLimit, $limit stays at userLimit (no optimization).
                const expectedLimitValue = expectOptimizedLimit ? effectiveLimit : userLimit;
                for (const limitStage of limitStages) {
                    assert.eq(
                        expectedLimitValue,
                        limitStage["$limit"],
                        `Expected $limit value ${expectedLimitValue}: ${tojson(result)}`,
                    );
                }
            }
        });
    }
});
