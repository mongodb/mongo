load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load("jstests/libs/optimizer_utils.js");      // For checkCascadesOptimizerEnabled.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

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
function assertApproxEq(expected, actual, msg, tolerance = 0.01) {
    assert(Math.abs(expected - actual) < tolerance, msg);
}

/**
 * Validates that the results and cardinality estimate for a given $match predicate agree.
 */
function verifyCEForMatch({coll, predicate, expected, ce, hint}) {
    const actual = coll.aggregate([{$match: predicate}], {hint}).toArray();
    assertArrayEq({actual, expected});

    const actualCE = getIxscanCEForMatch(coll, predicate, hint);
    const expectedCE = ce == undefined ? expected.length : ce;
    assertApproxEq(actualCE,
                   expectedCE,
                   `${tojson(predicate)} should have been estimated as ${expectedCE}, estimated ${
                       actualCE} instead.`);
}

/**
 * Validates that the generated histogram for the given 'coll' has the expected type counters.
 */
function createAndValidateHistogram({coll, expectedHistogram, empty = false}) {
    const field = expectedHistogram._id;
    const stats = db.system.statistics[coll.getName()];
    stats.drop();

    // We can't use forceBonsai here because the new optimizer doesn't know how to handle the
    // analyze command.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    // Set up histogram for test collection.
    const res = db.runCommand({analyze: coll.getName(), key: field});
    assert.commandWorked(res);

    // Validate histograms.
    const expected = empty ? [] : [expectedHistogram];
    const actual = stats.aggregate({$match: {_id: field}}).toArray();
    assertArrayEq({actual, expected});
}

/**
 * Useful boilerplate code for tests that need to use the analyze command and/or histogram
 * estimation. This ensures that the appropriate flags/query knobs are set and ensures the state of
 * relevant flags is restored after the test.
 */
function runHistogramsTest(test) {
    if (!checkCascadesOptimizerEnabled(db)) {
        jsTestLog("Skipping test because the optimizer is not enabled");
        return;
    }

    if (checkSBEEnabled(db, ["featureFlagSbeFull"], true)) {
        jsTestLog("Skipping the test because it doesn't work in Full SBE");
        return;
    }

    // We will be updating some query knobs, so store the old state and restore it after the test.
    const {internalQueryCardinalityEstimatorMode, internalQueryFrameworkControl} = db.adminCommand({
        getParameter: 1,
        internalQueryCardinalityEstimatorMode: 1,
        internalQueryFrameworkControl: 1,
    });

    try {
        test();
    } finally {
        // Reset query knobs to their original state.
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQueryCardinalityEstimatorMode,
            internalQueryFrameworkControl
        }));
    }
}

/**
 * We need to set the CE query knob to use histograms and force the use of the new optimizer to
 * ensure that we use histograms to estimate CE.
 */
function forceHistogramCE() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryCardinalityEstimatorMode: "histogram"}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
}
