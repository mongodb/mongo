load("jstests/libs/ce_stats_utils.js");
load("jstests/libs/optimizer_utils.js");
load("jstests/query_golden/libs/compute_errors.js");
load("jstests/query_golden/libs/generate_queries.js");

/**
 * Run the query specified in the 'testcase' document with the CE 'strategy'.
 */
function runAggregationWithCE(coll, testcase, strategy) {
    let explain = {};
    if (testcase["nReturned"] == null) {
        explain = coll.explain("executionStats").aggregate(testcase.pipeline);
        testcase["nReturned"] = explain.executionStats.nReturned;
    } else {
        explain = coll.explain().aggregate(testcase.pipeline);
    }
    testcase[strategy] = round2(getRootCE(explain));
}

/**
 * Run all queries in the array testCases with all CE strategies.
 */
function runQueries(coll, testCases, ceStrategies) {
    ceStrategies.forEach(function(strategy) {
        forceCE(strategy);
        testCases.forEach(testCase => runAggregationWithCE(coll, testCase, strategy));
    });
}

/**
 * Main function for CE accuracy testing for a collection in the 'testDB' specified in the
 * collection metadata. The function assumes that the collection exists and is populated with data.
 */
function runCETestForCollection(testDB, collMeta) {
    // Flag to show more information for debugging purposes:
    // - query results;
    // - execution of sampling CE strategy.
    const ceDebugFlag = false;
    let ceStrategies = ["heuristic", "histogram"];
    if (ceDebugFlag) {
        ceStrategies.push("sampling");
    }

    let collName = collMeta.collectionName;
    let coll = testDB[collName];
    const collSize = coll.find().itcount();
    print(`Running CE accuracy test for collection ${collName} of ${collSize} documents.\n`);

    // Collection to store accuracy errors.
    const errorColl = testDB.ce_errors;
    errorColl.drop();

    // Stats collection.
    const statsColl = testDB.system.statistics[collName];

    // Create indexes and statistics.
    let fields = [];
    let fieldTypes = [];
    for (const field of collMeta.fields) {
        fields.push(field.fieldName);
        fieldTypes.push(field.data_type);
    }

    // Switch to 'tryBonsai' to create statistics and generate queries.
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    createIndexes(coll, fields);
    analyzeFields(testDB, coll, fields);
    for (const field of fields) {
        const stats = statsColl.find({"_id": field})[0];
        assert.eq(stats["statistics"]["documents"], collSize, stats);
    }

    // Query generation for a given collection.
    // Queries are defined as documents. Example:
    // {_id: 1, pipeline: [{$match: {a: {$gt: 16}}}], "dtype": "int", "qtype" : "$gt"}.
    const sampleSize = 4;
    let testCases = generateQueries(coll, fields, fieldTypes, collSize, sampleSize, statsColl);

    runQueries(coll, testCases, ceStrategies);

    // Switch to 'tryBonsai' for accuracy analysis.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    // Compute CE errors for each query and strategy and populate the error collection 'errorColl'.
    populateErrorCollection(errorColl, testCases, ceStrategies, collSize);
    if (ceDebugFlag) {
        show(testCases);
        show(errorColl.find());
    }

    // Aggregate errors for all CE strategies per query category.
    aggregateErrorsPerCategory(errorColl, "qtype", ceStrategies, false);
    aggregateErrorsPerCategory(errorColl, "dtype", ceStrategies);

    printQueriesWithBadAccuracy(errorColl, testCases);
}
