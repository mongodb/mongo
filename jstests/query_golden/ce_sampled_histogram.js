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

const collData = 'ce_accuracy_test';
const histogramData = 'ce_sampled_histograms';
const dataDir = 'jstests/query_golden/libs/data/';
const sampleDB = db.getSiblingDB("ce_sampled_histogram");
const baseDB = db.getSiblingDB("ce_base_histogram");

load(`${dataDir}${collData}.schema`);  // For 'dbMetadata'.
load(`${dataDir}${collData}.data`);    // For 'dataSet'.

const collMetadata = dbMetadata[1];
const collName = collMetadata.collectionName;
assert.eq(collName, "ce_data_1000");

const sampleColl = sampleDB[collName];
const baseColl = baseDB[collName];

// This loads pre-generated histograms so that we can ensure we always consider the same sample for
// each test run.
load(`${dataDir}${histogramData}.data`);  // For 'frozenHistograms'.
const fields = frozenHistograms.map(histogram => histogram._id);

// Initialize base and sample collections with the same data.
loadJSONDataset(baseDB, dataSet, dbMetadata);
baseColl.aggregate({$out: {db: sampleDB.getName(), coll: collName}});
const collSize = baseColl.count();

/**
 * Returns a 2-element array containing the number of documents returned by the 'predicate' and the
 * cardinality estimate when run against 'coll'
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

const totSampleErr = {
    absError: 0,
    relError: 0,
    selError: 0
};
const totBaseErr = {
    absError: 0,
    relError: 0,
    selError: 0
};
function testMatchPredicate(baseColl, sampleColl, predicate) {
    // Determine number of documents returned and predicate CE for queries against base and sample
    // collections.
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

/**
 * Main testing function. Initializes histograms and then executes a series of queries against the
 * 'base' collection, whose histograms include all values, and the 'sampled' collection, whose
 * histograms include only 10% of values (initialized from 'frozenHistograms').
 */
runHistogramsTest(function testFrozenSampleHistogram() {
    // Initialize statistics for base collection such that we build a histogram on all values for
    // each relevant field.
    let projection = {_id: 0};
    const baseStatsColl = baseDB.system.statistics[collName];
    baseStatsColl.drop();
    for (const field of fields) {
        createHistogram(baseColl, field);
        projection = Object.assign(projection, {[field]: 1});
    }

    jsTestLog("Base histograms:");
    print(tojson(baseStatsColl.find().toArray()));

    // Initialize statistics for sampled collection using pre-generated histograms.
    const sampleStatsColl = sampleDB.system.statistics[collName];
    sampleStatsColl.drop();
    assert.commandWorked(sampleStatsColl.insert(frozenHistograms));

    // Run some queries to demonstrate that the sample CE scales to approach the base CE.
    const values = baseColl.find({_id: {$in: [3, 123, 1005]}}, projection).toArray();
    forceCE("histogram");
    let count = 0;
    for (const h of frozenHistograms) {
        const field = h._id;
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
