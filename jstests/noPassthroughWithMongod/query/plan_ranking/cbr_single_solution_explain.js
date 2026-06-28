/**
 * Test handling of queries with a single possible solution/plan, under CBR.
 */
import {
    getRejectedPlans,
    getWinningPlanFromExplain,
    getEngine,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, getCBRConfig, setCBRConfig} from "jstests/libs/query/cbr_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("CBR single-solution early exit", function () {
    const coll = db[jsTestName()];
    let savedConfig;

    // CBR configurations to exercise the early exit under to ensure behaviour is consistent.
    // 'cbrAlwaysCosted' indicates whether the strategy is guaranteed to produce CBR cost estimates
    // for a single-solution explain. CBRForNoMultiplanningResults may bypass CBR entirely when
    // multiplanning completes early (e.g. a COLLSCAN that exhausts the collection within the
    // trial-phase work budget), so it does not guarantee cost estimates.
    const configs = [
        {
            internalQueryCBRCEMode: "automaticCE",
            automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
            cbrAlwaysCosted: false,
        },
        {
            internalQueryCBRCEMode: "automaticCE",
            automaticCEPlanRankingStrategy: "CBRCostBasedRankerChoice",
            cbrAlwaysCosted: true,
        },
        {internalQueryCBRCEMode: "samplingCE", cbrAlwaysCosted: true},
        {internalQueryCBRCEMode: "heuristicCE", cbrAlwaysCosted: true},
    ];

    before(function () {
        savedConfig = getCBRConfig(db);
        coll.drop();
        const docs = [];
        for (let i = 0; i < 5000; i++) {
            docs.push({a: i, b: i});
        }
        assert.commandWorked(coll.insertMany(docs));
        // Indexes on 'a' and 'b' only. A predicate on 'c' cannot use any index, so the planner
        // produces a single COLLSCAN solution.
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
    });

    after(function () {
        setCBRConfig(db, savedConfig);
        coll.drop();
    });

    for (const config of configs) {
        const label = config.automaticCEPlanRankingStrategy
            ? `${config.internalQueryCBRCEMode}/${config.automaticCEPlanRankingStrategy}`
            : config.internalQueryCBRCEMode;

        describe(`with ${label}`, function () {
            before(function () {
                setCBRConfig(db, Object.assign({featureFlagCostBasedRanker: true}, config));
            });

            it("returns correct results for a single-solution query without explain", function () {
                // 'c' has no index, so there is exactly one candidate plan (COLLSCAN).
                assert.eq(coll.find({c: {$exists: false}}).itcount(), 5000, {config});
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
                    // CBRForNoMultiplanningResults may bypass CBR via the earlyExit path (when
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
