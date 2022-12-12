/**
 * Tests for cardinality estimation accuracy.
 * @tags: [
 *   requires_cqf,
 * ]
 */

(function() {

load("jstests/libs/ce_stats_utils.js");
load("jstests/libs/optimizer_utils.js");
load("jstests/query_golden/libs/ce_data.js");
load("jstests/query_golden/libs/compute_errors.js");
load("jstests/query_golden/libs/generate_queries.js");

runHistogramsTest(function testCEAccuracy() {
    // Flag to show more information for debugging purposes:
    // - query results;
    // - execution of sampling CE strategy.
    const ceDebugFlag = false;
    let ceStrategies = ["heuristic", "histogram"];
    if (ceDebugFlag) {
        ceStrategies.push("sampling");
    }

    const coll = db.ce_accuracy;
    coll.drop();

    // Collection to store accuracy errors.
    const errorColl = db.ce_errors;
    errorColl.drop();

    // Stats collection.
    const statsColl = db.system.statistics.ce_accuracy;

    jsTestLog("Populating collection");
    assert.commandWorked(coll.insertMany(getCEDocs()));
    const collSize = coll.find().itcount();
    print(`Collection count: ${coll.find().itcount()}\n`);
    if (ceDebugFlag) {
        show(coll.find().limit(100).toArray());
    }

    // Switch to 'tryBonsai' to create statistics.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    // Create indexes and statistics.
    const fields = ["a", "b", "c"];
    createIndexes(coll, fields);
    analyzeFields(coll, fields);
    for (const field of fields) {
        let stats = statsColl.find({"_id": field})[0];
        assert.eq(stats["statistics"]["documents"], collSize);
    }

    function runAggregationWithCE(testcase, strategy) {
        if (testcase["nReturned"] == null) {
            var explain = coll.explain("executionStats").aggregate(testcase.pipeline);
            testcase["nReturned"] = explain.executionStats.nReturned;
        } else {
            var explain = coll.explain().aggregate(testcase.pipeline);
        }
        testcase[strategy] = round2(getRootCE(explain));
    }

    // Run all queries in the array testCases with all CE strategies.
    function runQueries(testCases) {
        ceStrategies.forEach(function(strategy) {
            forceCE(strategy);
            testCases.forEach(testCase => runAggregationWithCE(testCase, strategy));
        });
    }

    function computeStrategyErrors(testcase, strategy, collSize) {
        let errorDoc = {};
        const absError = testcase[strategy] - testcase.nReturned;
        let relError = 0.0;
        if (testcase.nReturned > 0) {
            relError = absError / testcase.nReturned;
        } else if (testcase[strategy] > 0) {
            // We cannot compute the relative error by division by zero. Take 10% of the absolute
            // error instead.
            relError = 0.1 * testcase[strategy];
        }

        // Selectivity error wrt collection size.
        const selError = 100.0 * absError / collSize;
        errorDoc["absError"] = round2(absError);
        errorDoc["relError"] = round2(relError);
        errorDoc["selError"] = round2(selError);
        return errorDoc;
    }

    function computeAndPrintErrors(testcase, collSize) {
        let errorDoc = {_id: testcase._id};

        ceStrategies.forEach(function(strategy) {
            errors = computeStrategyErrors(testcase, strategy, collSize);
            errorDoc[strategy] = errors;
            print(`${strategy}: ${testcase[strategy]} `);
            print(`AbsError: ${errors["absError"]}, RelError: ${errors["relError"]} , SelError: ${
                errors["selError"]}%\n`);
        });
        return errorDoc;
    }

    // Queries.
    // Defined as documents: { _id: 1, pipeline: [{$match: {a: {$gt: 16}}}] }
    let values = selectQueryValues(coll, "a");
    let testCasesInt = generateComparisons("a", values);
    let testCasesStr = generateComparisons("b", ["", genRandomString(10), genRandomString(15)]);
    let testCasesArr = generateRangePredicates("c", [
        [2, 4],
    ]);
    let testCases = testCasesInt.concat(testCasesStr).concat(testCasesArr);

    let i = 0;
    for (let query of testCases) {
        query["_id"] = i++;
    }

    runQueries(testCases);

    // Compute errors per query.
    for (const testcase of testCases) {
        jsTestLog(`Query ${testcase._id}: ${tojsononeline(testcase.pipeline)}`);
        if (ceDebugFlag)
            show(coll.aggregate(testcase.pipeline));
        print(`Actual cardinality: ${testcase.nReturned}\n`);
        print(`Cardinality estimates:\n`);
        let errorDoc = computeAndPrintErrors(testcase, collSize);
        errorColl.insert(errorDoc);
    }

    if (ceDebugFlag) {
        show(testCases);
        show(errorColl.find());
    }

    // Switch to 'forceClassicEngine' to run the pipelines computing the errors.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

    // Compute aggregate errors for all CE strategies across all queries.
    const trialSize = errorColl.find().itcount();

    print(`\nHeuristic mean errors: `);
    print(`absRMSE: ${computeRMSE(errorColl, "$heuristic.absError", trialSize)}, `);
    print(`relRMSE: ${computeRMSE(errorColl, "$heuristic.relError", trialSize)}, `);
    print(
        `meanAbsSelErr: ${computeMeanAbsSelError(errorColl, "$heuristic.selError", trialSize)}%\n`);

    print(`Histogram mean errors: `);
    print(`absRMSE: ${computeRMSE(errorColl, "$histogram.absError", trialSize)}, `);
    print(`relRMSE: ${computeRMSE(errorColl, "$histogram.relError", trialSize)}, `);
    print(
        `meanAbsSelErr: ${computeMeanAbsSelError(errorColl, "$histogram.selError", trialSize)}%\n`);

    if (ceDebugFlag) {
        print(`Sampling mean errors: `);
        print(`absRMSE: ${computeRMSE(errorColl, "$sampling.absError", trialSize)}, `);
        print(`relRMSE: ${computeRMSE(errorColl, "$sampling.relError", trialSize)}, `);
        print(`meanAbsSelErr: ${
            computeMeanAbsSelError(errorColl, "$sampling.selError", trialSize)}%\n`);
    }
});
})();
