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

load('jstests/libs/ce_stats_utils.js');

const collName = "ce_histogram";
const fields = ["int", "dbl", "str", "date"];

let _id;

/**
 * Generates 'val' documents where each document has a distinct value for each 'field' in 'fields'.
 */
function generateDocs(val) {
    let docs = [];
    const fields = {
        int: NumberInt(val),  // Necessary to cast, otherwise we get a double here.
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
 * Returns the correct type name in the stats 'typeCount' field for the given field name.
 */
function getTypeName(field) {
    switch (field) {
        case "int":
            return "NumberInt32";
        case "dbl":
            return "NumberDouble";
        case "str":
            return "StringBig";
        case "date":
            return "Date";
        default:
            assert(false, `Name mapping for ${field} not defined.`);
    }
}

/**
 * This is the main testing function. Note that the input value 'ndv' corresponds to both the number
 * of distinct values per type in 'fields', as well as the number of buckets in each histogram
 * produced for this test.
 */
function verifyCEForNDV(ndv) {
    const coll = db[collName];
    coll.drop();

    const expectedHistograms = {};
    for (const field of fields) {
        assert.commandWorked(coll.createIndex({[field]: 1}));

        const typeName = getTypeName(field);
        expectedHistograms[field] = {
            _id: field,
            statistics: {
                documents: 0.0,
                scalarHistogram: {buckets: [], bounds: []},
                emptyArrayCount: 0.0,
                trueCount: 0.0,
                falseCount: 0.0,
                typeCount: [{typeName, count: 0.0}],
            }
        };
    }

    // Set up test collection and initialize the expected histograms in order to validate basic
    // histogram construction. We generate 'ndv' distinct values for each 'field', such that the
    // 'i'th distinct value has a frequency of 'i'. Because we have a small number of distinct
    // values, we expect to have one bucket per distinct value.
    _id = 0;
    let cumulativeCount = 0;
    let allDocs = [];
    for (let val = 1; val <= ndv; val++) {
        let docs = generateDocs(val);
        assert.commandWorked(coll.insertMany(docs));

        // Small hack; when we insert a doc, we want to insert it as a NumberInt so that the
        // appropriate type counters increment. However, when we verify it later on, we expect to
        // see a regular number, so we should update the "int" field of docs here.
        for (let doc of docs) {
            doc["int"] = val;
        }

        cumulativeCount += docs.length;
        for (const [f, expectedHistogram] of Object.entries(expectedHistograms)) {
            const {statistics} = expectedHistogram;
            statistics.documents = cumulativeCount;
            statistics.scalarHistogram.buckets.push({
                boundaryCount: val,
                rangeCount: 0,
                cumulativeCount,
                rangeDistincts: 0,
                cumulativeDistincts: val
            });
            statistics.scalarHistogram.bounds.push(docs[0][f]);
            statistics.typeCount[0].count += val;
        }
        allDocs = allDocs.concat(docs);
    }

    for (const field of fields) {
        createAndValidateHistogram({coll, expectedHistogram: expectedHistograms[field]});
        forceHistogramCE();

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

runHistogramsTest(function testScalarHistograms() {
    verifyCEForNDV(1);
    verifyCEForNDV(2);
    verifyCEForNDV(3);
    verifyCEForNDV(10);
});
}());
