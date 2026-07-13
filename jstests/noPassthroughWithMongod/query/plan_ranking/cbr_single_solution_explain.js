/**
 * Test handling of queries with a single possible solution/plan, under CBR.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {
    getRejectedPlans,
    getWinningPlanFromExplain,
    getEngine,
} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanCosted,
    getPlanRankerConfig,
    setPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("CBR single-solution early exit", function () {
    const coll = db[jsTestName()];
    let savedConfig;

    // CBR configurations to exercise the early exit under to ensure behaviour is consistent.
    // 'cbrAlwaysCosted' indicates whether the strategy is guaranteed to produce CBR cost estimates
    // for a single-solution explain. NoMultiplanningResults may bypass CBR entirely when
    // multiplanning completes early (e.g. a COLLSCAN that exhausts the collection within the
    // trial-phase work budget), so it does not guarantee cost estimates.
    const configs = [
        {
            internalQueryPlanRanker: "mixed",
            internalQueryCBRCEMode: "samplingCE",
            internalQueryMixedPlanRankingStrategy: "NoMultiplanningResults",
            cbrAlwaysCosted: false,
        },
        {
            internalQueryPlanRanker: "mixed",
            internalQueryCBRCEMode: "samplingCE",
            internalQueryMixedPlanRankingStrategy: "EstimateRankingEffort",
            cbrAlwaysCosted: true,
        },
        {
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
            cbrAlwaysCosted: true,
        },
        {
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "heuristicCE",
            cbrAlwaysCosted: true,
        },
    ];

    before(function () {
        savedConfig = getPlanRankerConfig(db);
        coll.drop();
        const docs = [];
        for (let i = 0; i < 1000; i++) {
            docs.push({a: i, b: i});
        }
        assert.commandWorked(coll.insertMany(docs));
        // Indexes on 'a' and 'b' only. A predicate on 'c' cannot use any index, so the planner
        // produces a single COLLSCAN solution.
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
    });

    after(function () {
        setPlanRankerConfig(db, savedConfig);
        coll.drop();
    });

    for (const config of configs) {
        const label = config.internalQueryMixedPlanRankingStrategy
            ? `${config.internalQueryCBRCEMode}/${config.internalQueryMixedPlanRankingStrategy}`
            : config.internalQueryCBRCEMode;

        describe(`with ${label}`, function () {
            before(function () {
                setPlanRankerConfig(db, Object.assign({featureFlagCostBasedRanker: true}, config));
            });

            it("returns correct results for a single-solution query without explain", function () {
                // 'c' has no index, so there is exactly one candidate plan (COLLSCAN).
                assert.eq(coll.find({c: {$exists: false}}).itcount(), 1000, {config});
                assert.eq(coll.find({c: 999}).itcount(), 0, {config});
            });

            it("explains a single-solution query and costs it with CBR", function () {
                const explain = coll.find({c: 999}).explain("allPlansExecution");
                const winningPlan = getWinningPlanFromExplain(explain);
                assert.neq(winningPlan, null, {explain});
                if (getEngine(explain) === "classic") {
                    // Single solution with no rejected plans.
                    assert.eq(getRejectedPlans(explain).length, 0, {explain});
                    // The !isExplain guard does not fire, so the ranking strategy runs.
                    // Strategies that always invoke CBR must surface costEstimate on the plan.
                    // NoMultiplanningResults may bypass CBR via the earlyExit path (when
                    // multiplanning finishes within its trial budget), so it is excluded.
                    if (config.cbrAlwaysCosted) {
                        assertPlanCosted(winningPlan, {explain});
                    }
                }
            });

            it("explains a single-solution fast count without ranking it", function () {
                // {a: {$gte: 0}} is fully covered by a count scan, which replaces all other plans,
                // leaving a single solution that must not be costed/ranked.
                const explain = coll
                    .explain("allPlansExecution")
                    .find({a: {$gte: 0}})
                    .count();
                const winningPlan = getWinningPlanFromExplain(explain);
                assert.neq(winningPlan, null, {explain});
                if (getEngine(explain) === "classic") {
                    assert.eq(getRejectedPlans(explain).length, 0, {explain});
                }
            });
        });
    }
});
