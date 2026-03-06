/**
 * Tests that $vectorSearch handles non-existent collections correctly.
 * - Non-explain queries should return no documents (EOF from doGetNext) even if UUID does not exist.
 * - Explain on non-sharded (mongod): fails with 65152 or 7828001 due to missing UUID.
 * - Explain on sharded (mongos): succeeds with EOF plan.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it} from "jstests/libs/mochalite.js";

const testDb = db.getSiblingDB(jsTestName());
const nonExistentCollName = jsTestName();

describe("$vectorSearch on non-existent collection", function () {
    it("should return no documents for non-explain query", function () {
        const pipeline = [
            {
                $vectorSearch: {
                    queryVector: [1.0, 2.0, 3.0],
                    path: "embedding",
                    numCandidates: 10,
                    limit: 5,
                    index: "vector_index",
                },
            },
        ];

        // Non-explain query should succeed but return no documents
        const res = assert.commandWorked(testDb.runCommand({aggregate: nonExistentCollName, pipeline, cursor: {}}));

        assert.eq(res.cursor.firstBatch, [], "Expected empty result set for vector search on non-existent collection");
        assert.eq(res.cursor.id, 0, "Expected cursor to be exhausted");
    });

    it("should fail with error for explain query on non-sharded; succeed with EOF on sharded", function () {
        const pipeline = [
            {
                $vectorSearch: {
                    queryVector: [1.0, 2.0, 3.0],
                    path: "embedding",
                    numCandidates: 10,
                    limit: 5,
                    index: "vector_index",
                },
            },
        ];

        const res = testDb.runCommand({aggregate: nonExistentCollName, pipeline, cursor: {}, explain: true});

        if (FixtureHelpers.isMongos(testDb)) {
            // Sharded: returns EOF explain for non-existent namespace.
            assert.commandWorked(res);
            assert.eq(res.queryPlanner.winningPlan.stage, "EOF", res);
            assert.eq(res.queryPlanner.winningPlan.type, "nonExistentNamespace", res);
        } else {
            // Non-sharded: explain requires collection UUID for $vectorSearch. Legacy vector search explain query should fail with error 7828001 (a valid collection is needed for explain).
            // Extension vector search should fail with error 65152 (EXPLAIN_COLLECTION_UUID_REQUIRED).
            assert.commandFailedWithCode(
                res,
                [65152, 7828001],
                "Expected error for missing UUID when explaining vector search on non-existent collection",
            );
        }
    });
});
