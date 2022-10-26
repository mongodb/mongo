/**
 * This is an integration test for histogram CE to ensure that we can create a histogram and
 * retrieve that histogram to estimate a simple match predicate. Note that this tests predicates and
 * histograms on several scalar types.
 *
 * This test is designed such that the constructed histograms should always give an exact
 * answer for a given equality predicate on any distinct value produced by 'generateDocs' below.
 * This is the case because the 'ndv' is sufficiently small such that each histogram will have
 * one bucket per distinct value with that distinct value as a bucket boundary. This should never
 * change as a result of updates to estimation, since estimates for bucket boundaries should always
 * be accurate.
 */
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load("jstests/libs/optimizer_utils.js");      // For checkCascadesOptimizerEnabled.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

const fields = ["int", "dbl", "str", "date"];
const tolerance = 0.01;

let _id;

// Helper functions.

/**
 * Generates 'val' documents where each document has a distinct value for each 'field' in 'fields'.
 */
function generateDocs(val) {
    let docs = [];
    const fields = {
        int: val,
        dbl: val + 0.1,
        // A small note: the ordering of string bounds (lexicographical) is different than that of
        // int bounds. In order to simplify the histogram validation logic, we don't want to have to
        // account for the fact that string bounds will be sorted differently than int bounds. To
        // illustrate this, if we were to use the format `string_${val}`, the string corresponding
        // to value 10 would be the second entry in the histogram bounds array, even though it would
        // be generated for 'val' = 10, not 'val' = 2.
        str: `string_${String.fromCharCode(64 + val)}`,
        date: new Date(`02 December ${val + 2000}`)
    };
    for (let i = 0; i < val; i++) {
        docs.push(Object.assign({_id}, fields));
        _id += 1;
    }
    return docs;
}

/**
 * Retrieves the cardinality estimate from a node in explain.
 */
function extractCEFromExplain(node) {
    const ce = node.properties.adjustedCE;
    assert.neq(ce, null);
    return ce;
}

/**
 * Extracts the cardinality estimate for the given $match predicate, assuming we get an index scan
 * plan.
 */
function getIxscanCEForMatch(coll, predicate, hint) {
    // We expect the plan to include a BinaryJoin whose left child is an IxScan whose logical
    // representation was estimated via histograms.
    const explain = coll.explain().aggregate([{$match: predicate}], {hint});
    const ixScan = leftmostLeafStage(explain);
    assert.neq(ixScan, null);
    assert.eq(ixScan.nodeType, "IndexScan");
    return extractCEFromExplain(ixScan);
}

/**
 * Asserts that expected and actual are equal, within a small tolerance.
 */
function assertApproxEq(expected, actual, msg) {
    assert(Math.abs(expected - actual) < tolerance, msg);
}

/**
 * Validates that the results and cardinality estimate for a given $match predicate agree.
 */
function verifyCEForMatch({coll, predicate, expected, hint}) {
    print("ACB", tojson(predicate));
    const actual = coll.aggregate([{$match: predicate}], {hint}).toArray();
    assertArrayEq({actual, expected});

    const expectedCE = expected.length;
    const actualCE = getIxscanCEForMatch(coll, predicate, hint);
    assertApproxEq(
        actualCE,
        expectedCE,
        `${tojson(predicate)} returned ${expectedCE} documents, estimated ${actualCE} docs.`);
}

/**
 * This is the main testing function. Note that the input value 'ndv' corresponds to both the number
 * of distinct values per type in 'fields', as well as the number of buckets in each histogram
 * produced for this test.
 */
function verifyCEForNDV(ndv) {
    /**
     * For this test we create one collection and with an index for each field. We use a new
     * collection name for each ndv and for each field because of two problems.
     *  1. SERVER-70855: Re-running the analyze command does update the statistics collection
     * correctly; however, CollectionStatistics caches a stale histogram generated for the previous
     * 'ndv'.
     *  2. SERVER-70856: We also can't currently have multiple histograms on a collection because
     * there is no logic to correctly filter on field name, which means we will always retrieve the
     * first histogram generated for the collection (regardless of which field we care about), even
     * though we have correct histograms in the system collection for all fields.
     *
     * TODO: rewrite this test to reuse the same collection once 1) & 2) are addressed.
     */
    for (const field of fields) {
        // We can't use forceBonsai here because the new optimizer doesn't know how to handle the
        // analyze command.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

        const collName = `ce_histogram_${field}_${ndv}`;
        const coll = db[collName];
        coll.drop();
        assert.commandWorked(coll.createIndex({[field]: 1}));

        const expectedHistograms = [];
        expectedHistograms.push(
            {_id: field, statistics: {documents: 0, scalarHistogram: {buckets: [], bounds: []}}});

        // Set up test collection and initialize the expected histograms in order to validate basic
        // histogram construction. We generate 'ndv' distinct values for each 'field', such that the
        // 'i'th distinct value has a frequency of 'i'. Because we have a small number of distinct
        // values, we expect to have one bucket per distinct value.
        _id = 0;
        let cumulativeCount = 0;
        let allDocs = [];
        for (let val = 1; val <= ndv; val++) {
            const docs = generateDocs(val);
            assert.commandWorked(coll.insertMany(docs));
            cumulativeCount += docs.length;
            for (const expectedHistogram of expectedHistograms) {
                const field = expectedHistogram._id;
                const {statistics} = expectedHistogram;
                statistics.documents = cumulativeCount;
                statistics.scalarHistogram.buckets.push({
                    boundaryCount: val,
                    rangeCount: 0,
                    cumulativeCount,
                    rangeDistincts: 0,
                    cumulativeDistincts: val
                });
                statistics.scalarHistogram.bounds.push(docs[0][field]);
            }
            allDocs = allDocs.concat(docs);
        }

        // Set up histogram for test collection.
        const stats = db.system.statistics[collName];
        stats.drop();

        const res = db.runCommand({analyze: collName, key: field});
        assert.commandWorked(res);

        // Validate histograms.
        const actualHistograms = stats.aggregate().toArray();
        assertArrayEq({actual: actualHistograms, expected: expectedHistograms});

        // We need to set the CE query knob to use histograms and force the use of the new optimizer
        // to ensure that we use histograms to estimate CE here.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryCardinalityEstimatorMode: "histogram"}));
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));

        // Verify CE for all distinct values of each field across multiple types.
        let count = 0;
        const hint = {[field]: 1};
        for (let val = 1; val <= ndv; val++) {
            // Compute the expected documents selected for a single range using the boundary val.
            const docsEq = allDocs.slice(count, count + val);
            const docsLt = allDocs.slice(0, count);
            const docsLte = allDocs.slice(0, count + val);
            const docsGt = allDocs.slice(count + val);
            const docsGte = allDocs.slice(count);

            for (const documentField of fields) {
                const fieldVal = docsEq[0][documentField];

                if (field == documentField) {
                    verifyCEForMatch(
                        {coll, predicate: {[field]: fieldVal}, hint, expected: docsEq});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lt: fieldVal}}, hint, expected: docsLt});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lte: fieldVal}}, hint, expected: docsLte});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gt: fieldVal}}, hint, expected: docsGt});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gte: fieldVal}}, hint, expected: docsGte});

                } else if (field == "int" && documentField == "dbl") {
                    // Each distinct double value corresponds to an int value + 0.1, so we shouldn't
                    // get any equality matches.
                    verifyCEForMatch({coll, predicate: {[field]: fieldVal}, hint, expected: []});

                    // When we have a predicate ~ < val + 0.1 or <= val + 0.1, it should match all
                    // integers <= val.
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lt: fieldVal}}, hint, expected: docsLte});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lte: fieldVal}}, hint, expected: docsLte});

                    // When we have a predicate ~ > val + 0.1 or >= val + 0.1, it should match all
                    // integers > val.
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gt: fieldVal}}, hint, expected: docsGt});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gte: fieldVal}}, hint, expected: docsGt});

                } else if (field == "dbl" && documentField == "int") {
                    // Each distinct double value corresponds to an int value + 0.1, so we shouldn't
                    // get any equality matches.
                    verifyCEForMatch({coll, predicate: {[field]: fieldVal}, hint, expected: []});

                    // When we have a predicate ~ < val - 0.1 or <= val - 0.1, it should match all
                    // doubles < val.
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lt: fieldVal}}, hint, expected: docsLt});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lte: fieldVal}}, hint, expected: docsLt});

                    // When we have a predicate ~ > val - 0.1 or >= val - 0.1, it should match all
                    // doubles >= val.
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gt: fieldVal}}, hint, expected: docsGte});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gte: fieldVal}}, hint, expected: docsGte});

                } else {
                    // Verify that we obtain a CE of 0 for types other than the 'field' type when at
                    // least one type is not numeric.
                    const expected = [];
                    verifyCEForMatch({coll, predicate: {[field]: fieldVal}, hint, expected});
                    verifyCEForMatch({coll, predicate: {[field]: {$lt: fieldVal}}, hint, expected});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$lte: fieldVal}}, hint, expected});
                    verifyCEForMatch({coll, predicate: {[field]: {$gt: fieldVal}}, hint, expected});
                    verifyCEForMatch(
                        {coll, predicate: {[field]: {$gte: fieldVal}}, hint, expected});
                }
            }

            count += val;
        }

        // Verify CE for values outside the range of distinct values for each field.
        const docLow = {int: 0, dbl: 0.0, str: `string_0`, date: new Date(`02 December ${2000}`)};
        const docHigh = generateDocs(ndv + 1)[0];
        const expected = [];
        verifyCEForMatch({coll, predicate: {[field]: docLow[field]}, hint, expected});
        verifyCEForMatch({coll, predicate: {[field]: docHigh[field]}, hint, expected});
    }
}

// Run test.

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

if (checkSBEEnabled(db, ["featureFlagSbeFull"], true)) {
    jsTestLog("Skipping the test because it doesn't work in Full SBE");
    return;
}

verifyCEForNDV(1);
verifyCEForNDV(2);
verifyCEForNDV(3);
verifyCEForNDV(10);
}());
