/**
 * Test that cost-based ranking can use sampling to estimate filters.
 */

import {
    getPlanStage,
    getRejectedPlans,
    getWinningPlanFromExplain,
    isCollscan,
} from "jstests/libs/query/analyze_plan.js";
import {getCBRConfig, setCBRConfig} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

let docs = [];
for (let i = 0; i < 1000; i++) {
    let doc = {a: i % 100 === 0 ? 0 : i, b: [i - 1, i, i + 1]};
    if (i % 2 === 0) {
        doc.c = i;
        doc.e = 0;
    }
    if (i % 100 === 0) {
        doc.d = "hello";
    }
    docs.push(doc);
}

assert.commandWorked(coll.insert(docs));

function assertCollscanUsesSampling(query) {
    const explain = coll.find(query).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan));
    assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
}

function assertAllPlansUseSampling(query, ce, allowedSources = ["Sampling"]) {
    const explain = coll.find(query).explain();
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach((plan) => {
        assert(
            allowedSources.includes(plan.estimatesMetadata.ceSource),
            `expected ceSource in ${tojson(allowedSources)}, got ${plan.estimatesMetadata.ceSource}: ${tojson(plan)}`,
        );
        if (ce === undefined) {
            assert.gt(plan.cardinalityEstimate, 0, plan);
        } else {
            assert.close(plan.cardinalityEstimate, ce, plan, 1);
        }
        if (plan.stage !== "COLLSCAN") {
            if (ce > 0) {
                assert.gt(plan.inputStage.cardinalityEstimate, 0, plan);
                assert.gt(plan.inputStage.numKeysEstimate, 0, plan);
            } else {
                assert.gte(plan.inputStage.cardinalityEstimate, 0, plan);
                assert.gte(plan.inputStage.numKeysEstimate, 0, plan);
            }
        }
    });
}

const prevCBRConfig = getCBRConfig(db);
const prevSamplingConfig = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalQueryDisablePlanCache: 1,
        internalQuerySamplingCEMethod: 1,
        internalQuerySamplingBySequentialScan: 1,
        samplingMarginOfError: 1,
    }),
);

try {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "samplingCE"}),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: 1}));
    assertCollscanUsesSampling({a: {$lt: 100}});
    assertCollscanUsesSampling({b: {$elemMatch: {$gt: 100, $lt: 200}}});
    // Test the chunk-based sampling method.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "chunk"}));
    assertCollscanUsesSampling({a: {$lt: 1000}});

    // Switch back to the random sampling method because the CE could be 0 for some of the filters
    // below, for example, the ce of 'b: {$lt: 500}' is 0 if all the chunks picked happen to fall
    // into the first half of the collection.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "random"}));

    // TODO SERVER-97790: Enable rooted $or test
    // assertCollscanUsesSampling({$or: [{a: {$lt: 100}}, {b: {$gt: 900}}]});

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));
    assertAllPlansUseSampling({a: {$lt: 100}});
    assertAllPlansUseSampling({b: {$lt: 100}});
    // TODO SERVER-100611: re-enable these tests.
    // assertAllPlansUseSampling({a: {$lt: 100}, b: {$lt: 500}});
    // assertAllPlansUseSampling({a: {$lt: 100}, b: {$lt: 500}, c: {$exists: true}});

    // Test that invalid query does not error during Sampling CE. Every sampled document throws
    // during evaluation, so the effective sample size is 0 and the estimate is reported with
    // source Code (an authoritative zero — see SamplingEstimatorImpl::makeScaledEstimate). After
    // merging with the Metadata-sourced collection cardinality during CE propagation the plan's
    // ceSource surfaces as Metadata. Authoritative zeros are preserved by
    // CardinalityEstimator::clampZeroEstimates.
    assertAllPlansUseSampling({$expr: {$divide: ["$a", 0]}}, 0, ["Metadata"]);

    // Test that an expression that causes an exception during evaluation is handled correctly when
    // evaluated during Sampling CE. Correct handling means that instead of throwing an exception
    // during CE, the exception is caught, the document is skipped, and the estimate is scaled
    // according to the number of successfully evaluated documents.
    // The test case is crafted so that there is a plan with an IndexScanNode that also has a
    // filter, and that filter contains an expression that would throw an excetion during
    // evaluation. The goal is to call SamplingEstimatorImpl::estimateRIDs with the IndexBounds of
    // this index scan, and trigger an exception. AND short-circuits so $expr is only evaluated
    // for docs where b==3 AND a==0, which no document satisfies — so the effective sample size
    // is positive, no match is observed, and the resulting Sampling-sourced zero is clamped by
    // CardinalityEstimator::clampZeroEstimates to kMinCE (1).
    assertAllPlansUseSampling({b: {$eq: 3}, a: {$eq: 0}, $expr: {$divide: ["$a", 0]}}, 1);

    // Similar to above, but division by a field value that is zero for some documents (every 100th
    // document has a=0), causing an exception during evaluation. Not every sampled document
    // throws, so the effective sample size is positive, no matches are observed, and the resulting
    // Sampling-sourced zero is clamped by CardinalityEstimator::clampZeroEstimates to kMinCE (1).
    assertAllPlansUseSampling({c: {$eq: 200}, a: {$eq: 0}, $expr: {$divide: [42, "$a"]}}, 1);

    db.adminCommand({setParameter: 1, samplingMarginOfError: 1.0});
    db.adminCommand({setParameter: 1, samplingConfidenceInterval: "99"});

    // Variant of the above: the expression errors only for some documents (those with a=0), while
    // others are evaluated successfully. The estimate is computed from non-error documents and
    // scaled by the effective sample size, producing a non-zero result.
    assertAllPlansUseSampling({b: {$gte: 0}, c: {$exists: true}, $expr: {$divide: [42, "$a"]}}, 494.949494949495);

    // Test that error-document scaling produces a correct estimate. The $expr throws for documents
    // with a=0 (10 documents at i=0,100,...,900). Of the remaining 990 evaluable documents,
    // 495 match {a: {$lt: 500}}, giving CE = 495 * 1000 / 990 = 500.
    assertAllPlansUseSampling({a: {$lt: 500}, $expr: {$divide: [42, "$a"]}}, 500);

    db.adminCommand({setParameter: 1, samplingMarginOfError: 5.0});
    db.adminCommand({setParameter: 1, samplingConfidenceInterval: "95"});

    // This valid query should get a positive ce.
    assertAllPlansUseSampling({$expr: {$divide: ["$a", 1]}});

    // Test the sequential scan sampling method that generates the sample by scanning the first N
    // documents of the collection. The sample generated by this method is repeatable as long as
    // the collection was populated in the same way.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}));
    // Run the same query multiple times to ensure the sample is repeatable.
    assertAllPlansUseSampling({a: {$lt: 100}}, 268.2291666666667);
    assertAllPlansUseSampling({a: {$lt: 100}}, 268.2291666666667);
    assertAllPlansUseSampling({a: {$lt: 100}}, 268.2291666666667);

    // Require a sample larger than the collection and test that a full scan of the collection was
    // done to collect the sample. With the effective sample covering every document,
    // SamplingEstimatorImpl::makeScaledEstimate tags the CE as 'Code' (authoritative),
    // so 'CardinalityEstimator::clampZeroEstimates' leaves any zero unchanged. COLLSCAN plans
    // multiply a Code-sourced selectivity by the Metadata-sourced collection cardinality, which
    // merges to 'Metadata'; index plans stay 'Code', so both are accepted below.
    assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 1.0}));
    const authoritative = ["Code", "Metadata"];
    // Since the sample was actually generated from all the documents in the collection. The 'ce' of
    // this predicate should include all documents, which is 1000.
    assertAllPlansUseSampling({a: {$lt: 1000}}, 1000, authoritative);

    // SERVER-109891 Check that $type, $ne, $regex works
    assertAllPlansUseSampling({a: {$type: "double"}}, 1000, authoritative);
    assertAllPlansUseSampling({a: {$ne: "test"}}, 1000, authoritative);
    assertAllPlansUseSampling({b: {$type: "double"}}, 1000, authoritative);
    assertAllPlansUseSampling({c: {$type: "double"}}, 500, authoritative);
    assertAllPlansUseSampling({e: {$ne: 0}}, 500, authoritative);

    // Queries that match no documents report CE 0 because the sampler observed every document
    // and found none matching - an authoritative zero that clampZeroEstimates preserves.
    assertAllPlansUseSampling({a: {$type: "string"}}, 0, authoritative);
    assertAllPlansUseSampling({d: {$type: "string"}}, 10, authoritative);
    assertAllPlansUseSampling({d: /^h/}, 10, authoritative);
    assertAllPlansUseSampling({d: /^e/}, 0, authoritative);

    // When sampling CE returns zero cardinality for every candidate index plan (because the
    // queried value is absent from the sample), the cost model — not enumeration order —
    // must decide which index wins. Verify this for several predicate shapes against a fixed
    // pair of indexes ({a:1} and {a:1, b:1}), in both index-creation orders.
    {
        // Reset sampling state to defaults so this test doesn't depend on prior test state. A
        // 5% margin of error keeps the sample size smaller than the collection populated below,
        // so the sampler's zero observation stays Sampling-sourced and CardinalityEstimator's
        // clamp differentiates plan costs by structural overhead.
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}));
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 5.0}));

        // A collection with no 'a' or 'b' fields forces sampling CE to return 0 for every
        // candidate index plan. Populate with enough documents that the sample is strictly
        // smaller than the collection - otherwise the sampler observes every document and the
        // zero is reported as an authoritative 'Code' source (see
        // SamplingEstimatorImpl::makeScaledEstimate), which clampZeroEstimates intentionally
        // leaves untouched.
        const zeroCEColl = db[collName + "_zero_ce"];
        const numDocs = 500;
        const docs = Array.from({length: numDocs}, (_, i) => ({c: i}));

        function assertIndexWins(query, expectedIndexName, projection) {
            const cursor = projection ? zeroCEColl.find(query, projection) : zeroCEColl.find(query);
            const winningPlan = getWinningPlanFromExplain(cursor.explain());
            const ixscan = getPlanStage(winningPlan, "IXSCAN");
            assert(ixscan, `no IXSCAN in winning plan: ${tojson(winningPlan)}`);
            assert.eq(ixscan.indexName, expectedIndexName, winningPlan);
        }

        function runZeroCEScenarios() {
            // Covered query on {a:1, b:1} must prefer the compound index.
            // The {a:1} plan would need a fetch with a residual filter on 'b'; the compound
            // index covers the full predicate with no residual filter.
            assertIndexWins({a: 1, b: 1}, "a_1_b_1", {_id: 0, a: 1, b: 1});

            // Same predicate without a projection forcing a covered plan: the compound index
            // must still win because the {a:1} plan still needs a residual filter on 'b'.
            assertIndexWins({a: 1, b: 1}, "a_1_b_1");

            // Predicate uses only the leading field. Both indexes are valid candidates, but
            // {a:1} is the narrower scan (fewer fields per key), so it must win over the
            // compound index.
            assertIndexWins({a: 1}, "a_1");

            // TODO SERVER-125783: enable once 'CardinalityEstimator::clampZeroEstimates' preserves
            // prefix-selectivity ordering across IXSCANs. With every approximate zero clamped to
            // the same kMinCE, the longer-prefix IXSCAN no longer has a smaller CE than the
            // shorter-prefix one, so the cost differential collapses to small calibrated terms
            // and CBR can pick the structurally weaker {a:1} plan over the compound index here.
            // assertIndexWins({a: 1, b: 1, c: 1}, "a_1_b_1");
        }

        // Run the same scenarios under both index-creation orders, since the bug fixed by
        // this PR depended on enumeration order — the buggy state would have CBR pick whichever
        // index happened to be enumerated first.
        for (const indexes of [
            [{a: 1}, {a: 1, b: 1}],
            [{a: 1, b: 1}, {a: 1}],
        ]) {
            zeroCEColl.drop();
            assert.commandWorked(zeroCEColl.insertMany(docs));
            for (const idx of indexes) {
                assert.commandWorked(zeroCEColl.createIndex(idx));
            }
            runZeroCEScenarios();
        }
        zeroCEColl.drop();
    }

    // Exact-zero CE is preserved by CardinalityEstimator::clampZeroEstimates when the source
    // is 'Code' (authoritative), which happens when the sampler observes the entire
    // collection and finds no matches (see SamplingEstimatorImpl::makeScaledEstimate). In
    // this regime per-node cardinalities collapse to zero, and it is the additive per-stage
    // minimum in the CostEstimator that differentiates structurally different plans.
    // Verify that the compound index still wins for such a query regardless of index creation
    // order, and that the costs of the two candidate plans are strictly positive and distinct.
    {
        // Drive the sampler to cover the whole collection so zero observations are tagged
        // as Code rather than Sampling.
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 1.0}));

        const exactZeroColl = db[collName + "_exact_zero_ce"];
        const numDocs = 50;
        const exactDocs = Array.from({length: numDocs}, (_, i) => ({c: i}));

        function assertCompoundIndexWinsExactZero() {
            const explain = exactZeroColl.find({a: 1, b: 1}, {_id: 0, a: 1, b: 1}).explain();
            const winningPlan = getWinningPlanFromExplain(explain);
            const rejectedPlans = getRejectedPlans(explain);
            assert.gt(rejectedPlans.length, 0, "expected at least one rejected plan to compare costs");

            // All plans must have strictly positive cost, and every rejected plan must cost
            // strictly more than the winner — otherwise the winner would depend on enumeration
            // order rather than cost.
            const winningCost = winningPlan.costEstimate;
            assert.gt(winningCost, 0, winningPlan);
            rejectedPlans.forEach((plan) => {
                assert.gt(
                    plan.costEstimate,
                    winningCost,
                    `expected rejected cost > winning: winning=${winningCost}, rejected=${plan.costEstimate}`,
                );
            });

            const indexName = winningPlan.inputStage.indexName
                ? winningPlan.inputStage.indexName
                : winningPlan.inputStage.inputStage.indexName;
            assert.eq(indexName, "a_1_b_1", winningPlan);
        }

        // Order 1: {a:1} created first.
        exactZeroColl.drop();
        assert.commandWorked(exactZeroColl.insertMany(exactDocs));
        assert.commandWorked(exactZeroColl.createIndex({a: 1}));
        assert.commandWorked(exactZeroColl.createIndex({a: 1, b: 1}));
        assertCompoundIndexWinsExactZero();

        // Order 2: {a:1, b:1} created first.
        exactZeroColl.drop();
        assert.commandWorked(exactZeroColl.insertMany(exactDocs));
        assert.commandWorked(exactZeroColl.createIndex({a: 1, b: 1}));
        assert.commandWorked(exactZeroColl.createIndex({a: 1}));
        assertCompoundIndexWinsExactZero();

        exactZeroColl.drop();
    }
} finally {
    setCBRConfig(db, prevCBRConfig);
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryDisablePlanCache: prevSamplingConfig.internalQueryDisablePlanCache,
            internalQuerySamplingCEMethod: prevSamplingConfig.internalQuerySamplingCEMethod,
            internalQuerySamplingBySequentialScan: prevSamplingConfig.internalQuerySamplingBySequentialScan,
            samplingMarginOfError: prevSamplingConfig.samplingMarginOfError,
        }),
    );
}
