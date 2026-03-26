/**
 * Test that cost-based ranking can use sampling to estimate filters.
 */

import {getRejectedPlans, getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {getCBRConfig, restoreCBRConfig} from "jstests/libs/query/cbr_utils.js";
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

function assertAllPlansUseSampling(query, ce) {
    const explain = coll.find(query).explain();
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach((plan) => {
        assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
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

    // Test that invalid query does not error during Sampling CE. 0 ce is returned because the query
    // always fails.
    assertAllPlansUseSampling({$expr: {$divide: ["$a", 0]}}, 0);

    // Test that an expression that causes an exception during evaluation is handled correctly when
    // evaluated during Sampling CE. Correct handling means that instead of throwing an exception
    // during CE, the exception is caught, the document is skipped, and the estimate is scaled
    // according to the number of successfully evaluated documents.
    // The test case is crafted so that there is a plan with an IndexScanNode that also has a
    // filter, and that filter contains an expression that would throw an excetion during
    // evaluation. The goal is to call SamplingEstimatorImpl::estimateRIDs with the IndexBounds of
    // this index scan, and trigger an exception.
    assertAllPlansUseSampling({b: {$eq: 3}, a: {$eq: 0}, $expr: {$divide: ["$a", 0]}}, 0);

    // Similar to above, but division by a field value that is zero for some documents (every 100th
    // document has a=0), causing an exception during evaluation.
    assertAllPlansUseSampling({c: {$eq: 200}, a: {$eq: 0}, $expr: {$divide: [42, "$a"]}}, 0);

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
    // done to collect the sample.
    assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 1.0}));
    // Since the sample was actually generated from all the documents in the collection. The 'ce' of
    // this predicate should include all documents, which is 1000.
    assertAllPlansUseSampling({a: {$lt: 1000}}, 1000);
    assertAllPlansUseSampling({a: {$lt: 1000}}, 1000);

    // SERVER-109891 Check that $type, $ne, $regex works
    assertAllPlansUseSampling({a: {$type: "double"}}, 1000);
    assertAllPlansUseSampling({a: {$ne: "test"}}, 1000);
    assertAllPlansUseSampling({b: {$type: "double"}}, 1000);
    assertAllPlansUseSampling({c: {$type: "double"}}, 500);
    assertAllPlansUseSampling({e: {$ne: 0}}, 500);

    assertAllPlansUseSampling({a: {$type: "string"}}, 0);
    assertAllPlansUseSampling({d: {$type: "string"}}, 10);
    assertAllPlansUseSampling({d: /^h/}, 10);
    assertAllPlansUseSampling({d: /^e/}, 0);
} finally {
    restoreCBRConfig(db, prevCBRConfig);
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
