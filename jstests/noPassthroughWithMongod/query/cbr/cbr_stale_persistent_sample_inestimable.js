/**
 * A persisted sample may contain documents that are incompatible with an index created later.
 * When samplingCE extracts keystrings from such a stale sample, the extraction fails.
 * The plan must become inestimable and fall back to the multi-planner instead of failing the query.
 *
 * @tags:[
 *   featureFlagPersistentStats,
 *   featureFlagCostBasedRanker
 * ]
 */

import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanNotCosted,
    getPlanRankerConfig,
    setPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db[collName];

const oldRankerConfig = getPlanRankerConfig(db);

describe("keystring extraction over a stale persisted sample", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(coll.insertOne({a: [0], b: 0}));

        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
            }),
        );

        // Persist a sample that includes the array-valued documents.
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
            }),
        );

        // Remove the arrays so the hashed index build can succeed.
        // Note that the persisted sample still contains them.
        // Insert one document the index can index so the collection is non-empty: with
        // an empty collection the estimator short-circuits on a zero input cardinality
        // and key extraction is never attempted.
        assert.commandWorked(coll.deleteMany({}));
        assert.commandWorked(coll.insertOne({a: 0, b: 0}));
        assert.commandWorked(coll.createIndex({a: "hashed"}));
    });

    after(function () {
        setPlanRankerConfig(db, oldRankerConfig);
    });

    it("makes the plan inestimable instead of failing the query", function () {
        // {b: 0} with the hint forces an unbounded ixscan, which returns full collection
        // cardinality without keystring extraction.
        // This tests that the estimator of the fetch node can catch key extraction exceptions
        // independently.
        for (const query of [{a: 0}, {b: 0}]) {
            const explain = coll.find(query).hint("a_hashed").explain("executionStats");
            const meta =
                explain.queryPlanner.ceSamplingMetadata[coll.getDB().getName() + "." + collName];
            assert.eq(meta.sampleSource, "persisted", "expected the persisted sample to serve", {
                meta,
            });

            // Key extraction over the stale sample must not fail the query: the plan
            // is inestimable and ranking falls to the multi-planner.
            const plans = [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)];
            plans.forEach((plan) => {
                assertPlanNotCosted(plan);
            });
            assert.eq(explain.executionStats.nReturned, 1, explain);
        }
    });
});
