/**
 * A non-first $rankFusion input pipeline gets wrapped in an internal $unionWith. When one of its
 * stages is an extension stage with no real backing BSON (only a placeholder), that $unionWith
 * must still build and run correctly instead of losing the stage on reparse.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagRankFusionFull,
 *   featureFlagVectorSimilarityExpressions,
 *   requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$rankFusion with an extension stage in a non-first input pipeline", function () {
    const collName = jsTestName();

    // Expands to: AST-only $matchTopNMetrics marker + host $match + $sort + $limit.
    const matchTopNWithMetricsStage = {
        $matchTopNWithMetrics: {filter: {x: {$gt: 2}}, sort: {x: -1}, limit: 3},
    };

    before(function () {
        db[collName].drop();

        assert.commandWorked(
            db[collName].insertMany([
                {_id: 0, x: 1, y: 9},
                {_id: 1, x: 5, y: 4},
                {_id: 2, x: 3, y: 7},
                {_id: 3, x: 8, y: 2},
            ]),
        );
    });

    it("$matchTopNWithMetrics as a non-first $rankFusion input pipeline builds and runs", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {a: [{$sort: {y: -1}}], b: [matchTopNWithMetricsStage]},
                    },
                },
            },
        ];
        const res = assert.commandWorked(
            db.runCommand({aggregate: collName, pipeline, cursor: {}}),
        );
        const results = res.cursor.firstBatch;
        // Pipeline "a" ($sort on the full collection) contributes all 4 docs; pipeline "b"
        // ($matchTopNWithMetrics, filtering x>2) contributes ids {1,2,3}. $rankFusion's result is
        // the union of documents seen by either input pipeline, so all 4 docs are expected.
        assert.sameMembers(
            [0, 1, 2, 3],
            results.map((doc) => doc._id),
            "expected the union of both input pipelines' documents",
            undefined,
            {results},
        );
    });
});
