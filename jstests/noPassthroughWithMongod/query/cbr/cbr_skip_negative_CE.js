/**
 * Regression test for AF-16798.
 *
 * estimate(SkipNode) compares `skip <= childEst` with fuzzy nearlyEqual (relative epsilon 1e-4)
 * but computes `childEst - skip` with exact double arithmetic. When skip > childEst in exact
 * arithmetic but the two are within the epsilon window, the subtraction produces a negative
 * cardinality, violating StrongDouble<Cardinality>::assertValid() (tassert 9274201).
 */

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-117707: Remove this check once CBR is enabled for SBE
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

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
