/**
 * Regression test for the CBR OIL multi-interval cardinality cap bug.
 * This may occur with either heuristicCE or histogramCE, but not samplingCE.
 *
 * estimate(OrderedIntervalList*) accumulates per-interval estimates:
 *   resultCard += sel * _inputCard  (for each interval)
 * and then caps the total:
 *   resultCard = std::min(resultCard, _inputCard)
 *
 * std::min on CardinalityEstimate uses fuzzy nearlyEqual (relative epsilon 1e-4) via
 * operator<=>. When resultCard > _inputCard exactly but within epsilon, the fuzzy min
 * considers them equivalent and silently returns resultCard (the first argument), so the
 * cap fails.
 *
 * The uncapped estimate then reaches:
 *   SelectivityEstimate sel = ceRes.getValue() / _inputCard;
 * in estimate(IndexBounds*). operator/(smaller_ce, bigger_ce) uses an exact double
 * comparison (tassert 9274202) rather than a fuzzy one, so it fires when
 * smaller_ce > bigger_ce exactly.
 *
 * With N=2600 docs and {a: {$in: [0, 50, 100, ..., 2500]}} (51 values), heuristic CE
 * computes for each point interval:
 *   sel = 1/sqrt(2600) per interval
 *   resultCard = 51 * sel * 2600 = 51 * sqrt(2600) ≈ 2600.4999
 *
 * Difference: 2600.4999 - 2600 ≈ 0.5
 * Relative:   0.5 / (2600.4999 + 2600) ≈ 9.61e-5 < 1e-4 (within fuzzy epsilon)
 *
 * So std::min returns 2600.4999 instead of 2600, and the subsequent division
 * 2600.4999 / 2600.0 fires tassert 9274202 ("must be <=").
 */

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// 2600 docs, a = 0..2599.
// The $in below uses 51 values spaced by 50 (0, 50, 100, ..., 2500), all present.
// With heuristic CE, each of the 51 point intervals in the OIL gets:
//   sel = 1/sqrt(2600) ≈ 0.019607843...
// Accumulated sum: 51 * (1/sqrt(2600)) * 2600 = 51 * sqrt(2600) ≈ 2600.4999
//   diff/sum = 0.4999 / (2600.4999 + 2600) ≈ 9.61e-5 < 1e-4 → within fuzzy epsilon
assert.commandWorked(coll.insertMany(Array.from({length: 2600}, (_, i) => ({a: i}))));
assert.commandWorked(coll.createIndex({a: 1}));

// 51 evenly-spaced values from 0..2500 (step 50)
const inValues = Array.from({length: 51}, (_, i) => i * 50);

try {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: true,
            internalQueryCBRCEMode: "heuristicCE",
        }),
    );

    // explain() bypasses the single-solution short-circuit in CBRPlanRankingStrategy::rankPlans()
    // (which skips cardinality estimation for non-explain single-candidate queries) and forces cost estimation to run for a single plan.
    //
    // Before the fix: tassert 9274202 fires because the fuzzy std::min cap on resultCard
    // in estimate(OIL*) lets 2600.4999 > 2600 escape, and the subsequent
    // ceRes.getValue() / _inputCard fires "X must be <= Y to produce selectivity".
    // After the fix: explain and execution both succeed.
    coll.find({a: {$in: inValues}}).explain();
    const result = coll.find({a: {$in: inValues}}).toArray();
    assert.eq(result.length, 51);
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: false,
        }),
    );
}
