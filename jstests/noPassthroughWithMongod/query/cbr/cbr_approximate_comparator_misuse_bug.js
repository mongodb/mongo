/**
 * Regression test for AF-16798 and other occurrences of the same bug.
 */

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-117707: Remove this check once CBR is enabled for SBE
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

/**
 * Case 1
 *
 * estimate(SkipNode) compares `skip <= childEst` with fuzzy nearlyEqual (relative epsilon 1e-4)
 * but computes `childEst - skip` with exact double arithmetic. When skip > childEst in exact
 * arithmetic but the two are within the epsilon window, the subtraction produces a negative
 * cardinality, violating StrongDouble<Cardinality>::assertValid() (tassert 9274201).
 */
(function testSkipNodeCE() {
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();

    // 1,000 docs, a = 0..999. Sequential sampling (internalQuerySamplingBySequentialScan)
    // scans the first S=384 documents in natural order (a=0..383). Of these, exactly 43 match
    // the filter {a: {$gte: 341}} (values a=341..383). The samplingCE estimate is:
    //
    //   estimate = matchCount * collCard / sampleSize = 43 * 1000 / 384 = 5375/48 ≈ 111.9791...
    //
    // With skip=112 and childEst=5375/48:
    //   diff/sum = (1/48) / (10751/48) = 1/10751 ≈ 9.3e-5 < 1e-4  →  nearlyEqual returns true
    // The guard `if (skip <= childEst)` passes via fuzzy equality, but the exact subtraction
    // childEst - skip = -1/48 triggers assertValid() (tassert 9274201).
    assert.commandWorked(coll.insertMany(Array.from({length: 1000}, (_, i) => ({a: i}))));
    assert.commandWorked(coll.createIndex({a: 1}));

    try {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                featureFlagCostBasedRanker: true,
                internalQueryCBRCEMode: "samplingCE",
                // Force sequential scan so the sample is the first 384 docs (a=0..383), making
                // the matchCount for {a: {$gte: 341}} deterministically 43.
                internalQuerySamplingBySequentialScan: true,
            }),
        );

        // explain() bypasses the single-solution short-circuit in CBRPlanRankingStrategy::rankPlans()
        // (which skips cardinality estimation for non-explain single-candidate queries), forcing CBR
        // to call estimatePlan() and reach estimate(SkipNode).
        // Before the fix: throws tassert 9274201
        // After the fix: explain and execution both succeed; the query returns results since
        // actual matching docs (a=341..999 = 659) > skip of 112.
        coll.find({a: {$gte: 341}})
            .skip(112)
            .explain();

        // Second test: skip == childEst exactly.
        // Of the first 384 sample docs (a=0..383), exactly 96 match {a: {$gte: 288}} (a=288..383).
        // The samplingCE estimate is:
        //
        //   estimate = matchCount * collCard / sampleSize = 96 * 1000 / 384 = 250.0
        //
        // 96 * 1000 = 96000 = 384 * 250, so the division is exact in double arithmetic.
        // With skip=250 and childEst=250.0:
        //   childEst - skip = 0.0  →  non-negative; tassert 9274201 is not triggered before or after fix.
        coll.find({a: {$gte: 288}})
            .skip(250)
            .explain();
    } finally {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                featureFlagCostBasedRanker: false,
                internalQuerySamplingBySequentialScan: false,
            }),
        );
    }
})();

/**
 * Case 2
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
 */
(function testIndexBoundsCE() {
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
})();
