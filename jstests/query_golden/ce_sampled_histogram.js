/**
 * Tests accuracy of pre-generated sample histograms against histograms built on the entire
 * collection.
 * @tags: [
 *   requires_cqf,
 * ]
 */

(function() {
load("jstests/libs/load_ce_test_data.js");            // For 'loadJSONDataset'.
load("jstests/libs/ce_stats_utils.js");               // For 'getRootCE', 'createHistogram'.
load("jstests/query_golden/libs/compute_errors.js");  // For 'computeStrategyErrors'.

Random.setRandomSeed(6345);

const collData = 'ce_accuracy_test';
const dataDir = 'jstests/query_golden/libs/data/';
const sampleRate = 0.1;

const sampleDB = db.getSiblingDB("ce_sampled_histogram");
const baseDB = db.getSiblingDB("ce_base_histogram");

load(`${dataDir}${collData}.schema`);  // For 'dbMetadata'.
load(`${dataDir}${collData}.data`);    // For 'dataSet'.

const collMetadata = dbMetadata[1];
const collName = collMetadata.collectionName;
assert.eq(collName, "ce_data_1000");

const sampleColl = sampleDB[collName];
const baseColl = baseDB[collName];

const fields = [
    "uniform_int_1000_1",
    "normal_int_1000_1",
    "chi2_int_1000_1",
    "mixed_int_uniform_1",
    "mixed_int_unf_norm_1",
    "mixed_int_unf_norm_chi_1",
    "mixed_int_unf_norm_chi_2",
    "choice_str_set_1112_33_norm_s",
    "choice_str_set_1112_33_norm_l",
    "choice_str_set_1112_1000_chi2_s",
    "choice_str_set_1112_1000_chi2_l"
];

// Initialize base collection.
loadJSONDataset(baseDB, dataSet, dbMetadata);
const collSize = baseColl.count();

/**
 * Main testing function. Initializes histograms and sample collection, and then executes a series
 * of queries against the 'base' collection, whose histograms include all values, and the 'sampled'
 * collection, whose histograms include only 10% of values.
 */
runHistogramsTest(function testSampleHistogram() {
    // Build histograms on the base collection.
    let projection = {_id: 0};
    for (const field of fields) {
        createHistogram(baseColl, field);
    }

    // Now select approximately 'sampleRate'*collSize documents from the base collection to insert
    // into the sample collection.
    let sample = [];
    for (let i = 0; i < collSize; i++) {
        if (Random.rand() < sampleRate) {
            sample.push(i);
        }
    }
    baseColl.aggregate({$match: {_id: {$in: sample}}},
                       {$out: {db: sampleDB.getName(), coll: collName}});

    // Build histograms on the sample.
    for (const field of fields) {
        createHistogram(sampleColl, field);
        projection = Object.assign(projection, {[field]: 1});
    }

    // Replace the sample coll with the full collection. In this way, we have a histogram on only a
    // sample of documents in the base collection. Note that this does not test $analyze sampling
    // logic, because that yields different results on every test run.
    baseColl.aggregate({$out: {db: sampleDB.getName(), coll: collName}});

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

    const totSampleErr = {absError: 0, relError: 0, selError: 0};
    const totBaseErr = {absError: 0, relError: 0, selError: 0};
    function testMatchPredicate(baseColl, sampleColl, predicate) {
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

        jsTestLog(`CE: ${tojson(predicate)}, base = ${baseCE}, sample = ${sampleCE}, actual = ${
            nReturned}`);
        print(`Base error: ${tojson(baseErr)}\n`);
        print(`Sample error: ${tojson(sampleErr)}`);
    }
    // Initialize statistics for base collection such that we build a histogram on all values for
    // each relevant field.
    const baseStatsColl = baseDB.system.statistics[collName];
    baseStatsColl.drop();
    for (const field of fields) {
        createHistogram(baseColl, field);
        projection = Object.assign(projection, {[field]: 1});
    }

    jsTestLog("Base histograms:");
    print(tojson(baseStatsColl.find().toArray()));

    jsTestLog("Sample histograms:");
    print(tojson(sampleDB.system.statistics[collName].find().toArray()));

    // Run some queries to demonstrate that the sample CE scales to approach the base CE.
    const values = baseColl.find({_id: {$in: [3, 123, 1005]}}, projection).toArray();
    forceCE("histogram");
    let count = 0;
    for (const field of fields) {
        for (let i = 1; i < values.length; i++) {
            const prev = values[i - 1][field];
            const cur = values[i][field];

            const min = prev < cur ? prev : cur;
            const max = prev > cur ? prev : cur;

            // Test a variety of queries.
            testMatchPredicate(baseColl, sampleColl, {[field]: {$gte: min, $lte: max}});
            testMatchPredicate(baseColl, sampleColl, {[field]: {$lt: min}});
            testMatchPredicate(baseColl, sampleColl, {[field]: {$eq: min}});
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
