/**
 * Tests accuracy of pre-generated sample histograms against histograms built on the entire
 * collection.
 * @tags: [
 *   requires_cqf,
 * ]
 */

/**
 * Returns a 2-element array containing the number of documents returned by the 'predicate' and
 * the cardinality estimate when run against 'coll'
 */
function getMatchCE(coll, predicate) {
    jsTestLog(`Query: ${coll.getName()} ${tojson(predicate)}`);
    const explain = coll.explain("executionStats").aggregate([{$match: predicate}]);
    const n = round2(explain.executionStats.nReturned);
    const ce = round2(getRootCE(explain));
    const explainSummarized = tojson(summarizeExplainForCE(explain));
    print(explainSummarized);
    return [n, ce];
}

function testMatchPredicate(baseColl, sampleColl, predicate, collSize, totSampleErr, totBaseErr) {
    // Determine number of documents returned and predicate CE for queries against base and
    // sample collections. The results should be the same in both cases since we reset the
    // sample collection to have the same documents as the base collection after building a
    // histogram on it.
    const [baseN, baseCE] = getMatchCE(baseColl, predicate);
    const [sampleN, sampleCE] = getMatchCE(sampleColl, predicate);
    assert.eq(baseN, sampleN);
    const nReturned = baseN;

    // Compute errors for each strategy compared to actual query cardinality.
    const baseErr = computeStrategyErrors({baseCE, nReturned}, "baseCE", collSize);
    const sampleErr = computeStrategyErrors({sampleCE, nReturned}, "sampleCE", collSize);

    totSampleErr.absError += sampleErr.absError;
    totSampleErr.relError += sampleErr.relError;
    totSampleErr.selError += sampleErr.selError;
    totBaseErr.absError += baseErr.absError;
    totBaseErr.relError += baseErr.relError;
    totBaseErr.selError += baseErr.selError;

    jsTestLog(
        `CE: ${tojson(predicate)}, base = ${baseCE}, sample = ${sampleCE}, actual = ${nReturned}`);
    print(`Base error: ${tojson(baseErr)}\n`);
    print(`Sample error: ${tojson(sampleErr)}`);
}

(function() {
load("jstests/libs/load_ce_test_data.js");  // For 'loadJSONDataset'.
load("jstests/libs/ce_stats_utils.js");     // For 'getRootCE', 'createHistogram', runHistogramsTest
load("jstests/query_golden/libs/compute_errors.js");  // For 'computeStrategyErrors'.

Random.setRandomSeed(6345);

const collData = 'ce_accuracy_test';
const dataDir = 'jstests/query_golden/libs/data/';
const sampleRate = 0.2;

load(`${dataDir}${collData}.schema`);  // For 'dbMetadata'.
load(`${dataDir}${collData}.data`);    // For 'dataSet'.

/**
 * Main testing function. Initializes histograms and sample collection, and then executes a series
 * of queries against the 'base' collection, whose histograms include all values, and the 'sampled'
 * collection, whose histograms include only 10% of values.
 */
runHistogramsTest(function testSampleHistogram() {
    const sampleDB = db.getSiblingDB("ce_sampled_histogram");
    const baseDB = db.getSiblingDB("ce_base_histogram");

    const collMetadata = dbMetadata[0];
    const collName = collMetadata.collectionName;
    assert.eq(collName, "ce_data_500");

    const sampleColl = sampleDB[collName];
    const baseColl = baseDB[collName];

    const fields = [
        "uniform_int_0-1000-1",
        "normal_int_0-1000-1",
        "chi2_int_0-1000-1",
        "mixdist_uniform_int_0-1000-1_uniform_int_7000-8000-1_normal_int_0-10000-10_",
        "mixdist_normal_int_0-1000-1_normal_int_0-10000-10_normal_int_0-100000-100_",
    ];

    // Initialize base collection.
    loadJSONDataset(baseDB, dataSet, dbMetadata);
    const collSize = baseColl.count();

    // Select approximately 'sampleRate'*collSize documents from the base collection to insert
    // into the sample collection.
    let sample = [];
    for (let i = 0; i < collSize; i++) {
        if (Random.rand() < sampleRate) {
            sample.push(i);
        }
    }
    baseColl.aggregate({$match: {_id: {$in: sample}}},
                       {$out: {db: sampleDB.getName(), coll: collName}});

    let projection = {_id: 0};
    let sortFields = {};
    // Build histograms on the base and sample collections.
    for (const field of fields) {
        projection = Object.assign(projection, {[field]: 1});
        sortFields = Object.assign(sortFields, {[field]: 1});
        createHistogram(baseColl, field);
        createHistogram(sampleColl, field);
    }

    // Replace the sample coll with the full collection. In this way, we have a histogram on only a
    // sample of documents in the base collection. Note that this does not test $analyze sampling
    // logic, because that yields different results on every test run.
    baseColl.aggregate({$out: {db: sampleDB.getName(), coll: collName}});

    // Run some queries to demonstrate that the sample CE scales to approach the base CE.
    const totSampleErr = {absError: 0, relError: 0, selError: 0};
    const totBaseErr = {absError: 0, relError: 0, selError: 0};

    forceCE("histogram");
    let count = 0;
    // Sort the values to ensure a stable test result.
    const values =
        baseColl.find({_id: {$in: [3, 123, 405]}}, projection).sort(sortFields).toArray();
    for (const field of fields) {
        for (let i = 1; i < values.length; i++) {
            const prev = values[i - 1][field];
            const cur = values[i][field];

            const min = prev < cur ? prev : cur;
            const max = prev > cur ? prev : cur;

            // Test a variety of queries.
            testMatchPredicate(baseColl,
                               sampleColl,
                               {[field]: {$gte: min, $lte: max}},
                               collSize,
                               totSampleErr,
                               totBaseErr);
            testMatchPredicate(
                baseColl, sampleColl, {[field]: {$lt: min}}, collSize, totSampleErr, totBaseErr);
            testMatchPredicate(
                baseColl, sampleColl, {[field]: {$eq: min}}, collSize, totSampleErr, totBaseErr);
            count += 3;
        }
    }

    const avgBaseErr = {
        absError: round2(totBaseErr.absError / count),
        relError: round2(totBaseErr.relError / count),
        selError: round2(totBaseErr.selError / count)
    };
    const avgSampleErr = {
        absError: round2(totSampleErr.absError / count),
        relError: round2(totSampleErr.relError / count),
        selError: round2(totSampleErr.selError / count)
    };

    jsTestLog(`Average errors (${count} queries):`);
    print(`Average base error: ${tojson(avgBaseErr)}\n`);
    print(`Average sample error: ${tojson(avgSampleErr)}`);
});
})();
