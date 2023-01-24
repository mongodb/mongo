/**
 * This is an integration test for histogram CE & statistics to ensure that we can estimate a
 * histogram appropriately for different sample sizes.
 */
(function() {
"use strict";

load('jstests/libs/ce_stats_utils.js');

const field = "sampled";
const numDocs = 1000;

function generateDocs(getVal) {
    let out = [];
    // We generate an extremely simple collection in order to be able to get exactly the same
    // results for different sample sizes and thus verify that CE scales with sample size.
    for (let i = 0; i < numDocs; i++) {
        out.push({[field]: getVal(i)});
    }
    return out;
}

function initColl(coll, docs) {
    coll.drop();
    assert.commandWorked(coll.createIndex({[field]: 1}));
    assert.commandWorked(coll.insertMany(docs));
}

const coll = db.sample_rate;

function compareCEAndActualN(coll, predicate, expectedCE, tolerance = 0.01) {
    const explain = coll.explain("executionStats").aggregate([{$match: {[field]: predicate}}]);
    const ce = round2(getRootCE(explain));
    const n = explain.executionStats.nReturned;
    const msg = `Expected CE ${expectedCE} for predicate ${tojson(predicate)},` +
        ` estimate ${ce} differed by more than ${tolerance} (${n} documents actually returned)`;
    assertApproxEq(expectedCE, ce, msg, tolerance);
}

function testPredicates(coll, expectedEst) {
    forceCE("histogram");
    for (const {predicate, ce, tolerance} of expectedEst) {
        compareCEAndActualN(coll, predicate, ce, tolerance);
    }
}

function testSampleRatesForDocsWithPredicates(docs, expectedEst, expectedHistogram, sampleRates) {
    initColl(coll, docs);

    // Note: we should always obtain the same histogram when we have a sample rate of 1.0.
    createAndValidateHistogram({coll, expectedHistogram});
    testPredicates(coll, expectedEst);

    // We cannot rely on getting the same sample every time we run analyze, so we should not
    // validate the histogram here, since we will get different buckets. We also can't predict the
    // exact sample size.
    for (const sampleRate of sampleRates) {
        const sampleSize = docs.length * sampleRate;
        jsTestLog(`Testing ${sampleRate}`);
        createHistogram(coll, field, {sampleRate});
        testPredicates(coll, expectedEst);

        jsTestLog(`Testing ${sampleSize}`);
        createHistogram(coll, field, {sampleSize});
        testPredicates(coll, expectedEst);
    }
}

/**
 * Tests that when we vary the sample rate on collections with an extremely simple distribution, the
 * generated histogram estimates are close to what we would expect for a histogram built on the
 * entire collection.
 */
runHistogramsTest(function testSampleRates() {
    // Test a scalar histogram with only one value (50).
    {
        const docs = generateDocs(() => 50);
        const sampleRates = [0.01, 0.1, 0.5];
        const expectedEst = [
            {predicate: {$eq: 50}, ce: numDocs},
            {predicate: {$lt: 75}, ce: numDocs},
            {predicate: {$gt: 75}, ce: 0},
            {predicate: {$gt: 25, $lt: 75}, ce: numDocs},
        ];
        const expectedHistogram = {
            _id: field,
            statistics: {
                documents: numDocs,
                trueCount: 0,
                falseCount: 0,
                sampleRate: 1.0,
                emptyArrayCount: 0,
                typeCount: [{typeName: "NumberDouble", count: numDocs}],
                scalarHistogram: {
                    buckets: [
                        {
                            boundaryCount: numDocs,
                            rangeCount: 0,
                            rangeDistincts: 0,
                            cumulativeCount: numDocs,
                            cumulativeDistincts: 1
                        },
                    ],
                    bounds: [50]
                }
            }
        };
        testSampleRatesForDocsWithPredicates(docs, expectedEst, expectedHistogram, sampleRates);
    }

    // Test statistics for a collection containing only booleans (therefore using type counts).
    {
        const trueCount = 990;
        const falseCount = 10;
        const docs = generateDocs((i) => i >= falseCount);
        const sampleRates = [0.1, 0.5, 0.99];
        const expectedEst = [
            {predicate: {$eq: true}, ce: trueCount, tolerance: 100},
            {predicate: {$eq: false}, ce: falseCount, tolerance: 100}
        ];
        const expectedHistogram = {
            _id: field,
            statistics: {
                documents: numDocs,
                trueCount: trueCount,
                falseCount: falseCount,
                sampleRate: 1.0,
                emptyArrayCount: 0,
                typeCount: [{typeName: "Boolean", count: numDocs}],
                scalarHistogram: {buckets: [], bounds: []}
            }
        };
        testSampleRatesForDocsWithPredicates(docs, expectedEst, expectedHistogram, sampleRates);
    }
});
})();
