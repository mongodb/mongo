load("jstests/libs/ce_stats_utils.js");
load("jstests/libs/optimizer_utils.js");
load("jstests/query_golden/libs/compute_errors.js");
load("jstests/query_golden/libs/generate_queries.js");

function indexedStrategy(strategyName) {
    return strategyName + "Idx";
}

function timedExplain(coll, pipeline) {
    const t0 = Date.now();
    explain = coll.explain().aggregate(pipeline);
    const t1 = Date.now();
    const duration = t1 - t0;
    return {explain, duration};
}

function getCE(pipeline, explain) {
    try {
        return round2(getRootCE(explain));
    } catch (e) {
        print(`Unable to retrieve root CE\nQuery: ${tojsononeline(pipeline)}`);
        printjson(explain);
        throw e;
    }
}

/**
 * Run the query specified in the 'testcase' document with the CE 'strategy'.
 */
function runAggregationWithCE(coll, testcase, strategy) {
    let explain = {};
    let duration = -1;
    if (testcase["nReturned"] == null) {
        try {
            explain = coll.explain("executionStats").aggregate(testcase.pipeline);
        } catch (error) {
            print(`runAggregationWithCE(coll=${coll}, testcase=${tojson(testcase)}, strategy=${
                strategy})`);
        }
        testcase["nReturned"] = explain.executionStats.nReturned;
        // Run explain without execution to measure optimization time. Ignore the explain.
        timedRes = timedExplain(coll, testcase.pipeline);
        duration = timedRes.duration;
    } else {
        timedRes = timedExplain(coll, testcase.pipeline);
        explain = timedRes.explain;
        duration = timedRes.duration;
    }
    assert.neq(duration, -1);
    assert.neq(explain, {});
    assert.gte(testcase["nReturned"], 0);
    testcase[strategy] = getCE(testcase.pipeline, explain);
    testcase['duration_' + strategy] = duration;
}

/**
 * Run queries with complex predicates in batches with a limited number of index fields.
 */
function runComplexPredicates(coll, testCases, ceStrategies, ceDebugFlag) {
    const maxIndexCnt = 50;
    let start = 0;

    while (start < testCases.length) {
        let end = start;
        let fieldsToIndex = new Set();
        // Detect the end of the batch such that no more than maxIndexCnt indexes are needed.
        while (end < testCases.length &&
               fieldsToIndex.size + testCases[end].fieldNames.length < maxIndexCnt) {
            testCases[end].fieldNames.forEach(function(field) {
                fieldsToIndex.add(field);
            });
            end++;
        }
        // Index all fields used in the batch of queries [start, end).
        print(`Running query batch [${start} - ${end}) with fields ${tojson(fieldsToIndex)}\n`);
        for (const field of fieldsToIndex) {
            assert.commandWorked(coll.createIndex({[field]: 1}));
        }
        ceStrategies.forEach(function(strategy) {
            forceCE(strategy);
            for (let i = start; i < end; i++) {
                let testcase = testCases[i];
                const {explain, duration} = timedExplain(coll, testcase.pipeline);
                testcase[indexedStrategy(strategy)] = getCE(testcase.pipeline, explain);
                testcase['duration_' + indexedStrategy(strategy)] = duration;
                if (ceDebugFlag == true && strategy == "histogram") {
                    print(`\nQuery plan for #${testcase._id}: ${tojsononeline(testcase.pipeline)}`);
                    printjson(getPlanSkeleton(navigateToRootNode(explain),
                                              {extraKeepKeys: ["interval"]}));
                }
            }
        });
        for (const field of fieldsToIndex) {
            assert.commandWorked(coll.dropIndex({[field]: 1}));
        }
        start = (start == end) ? start + 1 : end;
    }
}

/**
 * Run all queries in the array testCases with all CE strategies.
 * If 'fields' is not empty, create index for each field and execute all queries on this field.
 * If 'fields' is empty, execute queries with complex predicates in batches.
 */
function runQueries(coll, testCases, ceStrategies, fields, ceDebugFlag) {
    print("Run queries without indexing.\n");
    ceStrategies.forEach(function(strategy) {
        forceCE(strategy);
        testCases.forEach(testCase => runAggregationWithCE(coll, testCase, strategy));
    });

    print("Run queries with indexed fields.\n");
    if (fields.length > 0) {
        ceStrategies.forEach(function(strategy) {
            forceCE(strategy);
            for (const field of fields) {
                assert.commandWorked(coll.createIndex({[field]: 1}));
                for (let testcase of testCases) {
                    if (testcase.fieldName === field) {
                        const {explain, duration} = timedExplain(coll, testcase.pipeline);
                        testcase[indexedStrategy(strategy)] = getCE(testcase.pipeline, explain);
                        testcase['duration_' + indexedStrategy(strategy)] = duration;
                    }
                }
                assert.commandWorked(coll.dropIndex({[field]: 1}));
            }
        });
    } else {
        runComplexPredicates(coll, testCases, ceStrategies, ceDebugFlag);
    }
}

function printSimpleQueryStats(errorColl, strategies, queries, debugFlag) {
    jsTestLog("Aggregate errors for all simple predicate queries");

    // Aggregate errors for all CE strategies per query category.
    aggregateErrorsPerCategory(errorColl, ["qtype"], strategies);
    aggregateErrorsPerCategory(errorColl, ["qtype", "dtype"], strategies);

    // Aggregate errors for all CE strategies per data type and distribution.
    aggregateErrorsPerCategory(errorColl, ["dtype"], strategies);
    aggregateErrorsPerCategory(errorColl, ["distr", "dtype"], strategies);

    // Aggregate scalar and array fields query errors per strategy.
    aggregateErrorsPerStrategy(errorColl, strategies, {"dtype": {$ne: "array"}});
    aggregateErrorsPerStrategy(errorColl, strategies, {"dtype": "array"});
    aggregateErrorsPerStrategy(
        errorColl, strategies, {$and: [{"elemMatch": true}, {"dtype": "array"}]});
    aggregateErrorsPerStrategy(
        errorColl, strategies, {$and: [{"elemMatch": false}, {"dtype": "array"}]});

    // Aggregate errors per strategy.
    aggregateErrorsPerStrategy(errorColl, strategies);

    printQueriesWithBadAccuracy(errorColl, queries, "histogram", "relError");
    printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("histogram"), "relError");
    printQueriesWithBadAccuracy(errorColl, queries, "histogram", "qError");
    printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("histogram"), "qError");
    if (debugFlag) {
        printQueriesWithBadAccuracy(errorColl, queries, "sampling", "relError");
        printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("sampling"), "relError");
        printQueriesWithBadAccuracy(errorColl, queries, "sampling", "qError");
        printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("sampling"), "qError");
    }
}

function printComplexQueryStats(errorColl, strategies, queries, debugFlag) {
    jsTestLog("Aggregate errors for all complex predicate queries");
    // Aggregate errors for all CE strategies per query category.
    aggregateErrorsPerCategory(errorColl, ["qtype"], strategies);
    aggregateErrorsPerCategory(errorColl, ["qtype", "numberOfTerms"], strategies);
    aggregateErrorsPerCategory(errorColl, ["numberOfTerms"], strategies);

    aggregateErrorsPerStrategy(errorColl, strategies);

    printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("histogram"), "relError");
    printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("histogram"), "qError");
    if (debugFlag) {
        printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("sampling"), "relError");
        printQueriesWithBadAccuracy(errorColl, queries, indexedStrategy("sampling"), "qError");
    }
}

function printAllQueryStats(testDB, errorColl1, errorColl2, strategies) {
    jsTestLog("Aggregate errors for all queries (simple and complex predicates)");
    let allErrorsColl = testDB.ce_all_errors;
    allErrorsColl.drop();
    allErrorsColl.insertMany(errorColl1.find({}, {_id: 0}).toArray());
    allErrorsColl.insertMany(errorColl2.find({}, {_id: 0}).toArray());
    aggregateErrorsPerStrategy(allErrorsColl, strategies);
}

/**
 * Main function for CE accuracy testing for a collection in the 'testDB' specified in the
 * collection metadata. The function assumes that the collection exists and is populated with data.
 * 'sampleSize' is the number of documents used to extract sample values for query generation.
 */
function runCETestForCollection(testDB, collMeta, sampleSize = 6, ceDebugFlag = false) {
    let ceStrategies = ["heuristic", "histogram"];
    if (ceDebugFlag) {
        ceStrategies.push("sampling");
    }

    let collName = collMeta.collectionName;
    let coll = testDB[collName];
    const collSize = coll.find().itcount();
    print(`Running CE accuracy test for collection ${collName} of ${collSize} documents.\n`);

    // Create statistics.
    let fields = [];
    let fieldTypes = [];
    for (const field of collMeta.fields) {
        fields.push(field.fieldName);
        fieldTypes.push(field.dataType);
    }

    // Switch to 'tryBonsai' to create statistics and generate queries.
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    analyzeFields(testDB, coll, fields);
    const statsColl = testDB.system.statistics[collName];

    for (const field of fields) {
        const stats = statsColl.find({"_id": field})[0];
        assert.eq(stats["statistics"]["documents"], collSize, stats);
    }

    // Query generation for a given collection.
    print("Begin query generation");
    // Select values to be used in queries from collection sample, min/max, and histogram bucket
    // boundaries.
    const samplePos = selectSamplePos(collSize, sampleSize);
    const queryValues = selectQueryValues(coll, fields, fieldTypes, samplePos, statsColl);

    // Queries are defined as documents. Example:
    // {_id: 1, pipeline: [{$match: {a: {$gt: 16}}}], "dtype": "int", "qtype" : "$gt"}.
    let testCases = generateQueries(fields, fieldTypes, queryValues);
    let testCasesColl = testDB['testCases'];
    testCasesColl.drop();
    testCasesColl.insertMany(testCases);  // Store them for future use.
    print(`Running ${testCases.length} simple predicate queries over ${fields.length} fields.\n`);
    runQueries(coll, testCases, ceStrategies, fields, ceDebugFlag);

    // Complex predicates
    jsTestLog("Running queries with complex predicates");
    let complexPred = generateComplexPredicates(testCases, fields, fieldTypes, queryValues);
    let complexPredColl = testDB['complexPred'];
    complexPredColl.drop();
    complexPredColl.insertMany(complexPred);  // Store them for future use.
    print(`Running ${complexPred.length} queries with complex predicates.\n`);
    runQueries(coll, complexPred, ceStrategies, [], ceDebugFlag);

    // Switch to 'tryBonsai' for accuracy analysis.
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    let allStrategies = [];
    for (let strategy of ceStrategies) {
        allStrategies.push(strategy);
        allStrategies.push(indexedStrategy(strategy));
    }

    // Compute CE errors per strategy for individual queries in 'testCases' and populate the error
    // collection 'errorColl'.
    let errorColl = testDB.ce_errors;
    errorColl.drop();
    populateErrorCollection(errorColl, testCases, allStrategies, collSize, false);
    assert.eq(testCases.length, errorColl.find().itcount());

    // Compute CE errors per strategy for individual queries in 'complexPred' and populate the error
    // collection 'errorCollComplexPred'.
    let errorCollComplexPred = testDB.ce_errors_complex_pred;
    errorCollComplexPred.drop();
    populateErrorCollection(errorCollComplexPred, complexPred, allStrategies, collSize, true);
    assert.eq(complexPred.length, errorCollComplexPred.find().itcount());

    // Print all collected statistics about estimation errors.
    printSimpleQueryStats(errorColl, allStrategies, testCases, ceDebugFlag);
    printComplexQueryStats(errorCollComplexPred, allStrategies, complexPred, ceDebugFlag);
    printAllQueryStats(testDB, errorColl, errorCollComplexPred, allStrategies);

    // Print all collected statistics about estimation errors excluding queries with empty result.
    print('===============================================================================');
    print('Errors excluding empty queries.');
    testDB.createView(
        'ce_errors_not_empty', 'ce_errors', [{$match: {$expr: {$gt: ["$nReturned", 0]}}}]);
    testDB.createView('ce_errors_complex_pred_not_empty',
                      'ce_errors_complex_pred',
                      [{$match: {$expr: {$gt: ["$nReturned", 0]}}}]);
    errorCollNonEmpty = testDB.ce_errors_not_empty;
    errorCollComplexPredNonEmpty = testDB.ce_errors_complex_pred_not_empty;
    print(`Non-empty simple error entries: ${
        errorCollNonEmpty.find().itcount()}; complex error entries: ${
        errorCollComplexPredNonEmpty.find().itcount()}`);
    printSimpleQueryStats(errorCollNonEmpty, allStrategies, testCases, ceDebugFlag);
    printComplexQueryStats(errorCollComplexPredNonEmpty, allStrategies, complexPred, ceDebugFlag);
    printAllQueryStats(testDB, errorCollNonEmpty, errorCollComplexPredNonEmpty, allStrategies);

    if (ceDebugFlag) {
        jsTestLog("Optimization times for simple predicates:");
        aggegateOptimizationTimesPerStrategy(errorColl, allStrategies);
        jsTestLog("Optimization times for simple complex predicates:");
        aggegateOptimizationTimesPerStrategy(errorCollComplexPred, allStrategies);
    }
}
