load("jstests/libs/ce_stats_utils.js");
load("jstests/libs/optimizer_utils.js");
load("jstests/query_golden/libs/compute_errors.js");
load("jstests/query_golden/libs/generate_queries.js");

function indexedStrategy(strategyName) {
    return strategyName + "Idx";
}

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
                const explain = coll.explain().aggregate(testcase.pipeline);
                testcase[indexedStrategy(strategy)] = round2(getRootCE(explain));
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
    // Run queries without indexing.
    ceStrategies.forEach(function(strategy) {
        forceCE(strategy);
        testCases.forEach(testCase => runAggregationWithCE(coll, testCase, strategy));
    });

    // Run queries with indexed fields.
    if (fields.length > 0) {
        ceStrategies.forEach(function(strategy) {
            forceCE(strategy);
            for (const field of fields) {
                assert.commandWorked(coll.createIndex({[field]: 1}));
                for (let testcase of testCases) {
                    if (testcase.fieldName === field) {
                        const explain = coll.explain().aggregate(testcase.pipeline);
                        testcase[indexedStrategy(strategy)] = round2(getRootCE(explain));
                    }
                }
                assert.commandWorked(coll.dropIndex({[field]: 1}));
            }
        });
    } else {
        runComplexPredicates(coll, testCases, ceStrategies, ceDebugFlag);
    }
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

    // Collection to store accuracy errors.
    const errorColl = testDB.ce_errors;
    errorColl.drop();

    // Stats collection.
    const statsColl = testDB.system.statistics[collName];

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
    for (const field of fields) {
        const stats = statsColl.find({"_id": field})[0];
        assert.eq(stats["statistics"]["documents"], collSize, stats);
    }

    // Query generation for a given collection.
    // Select values to be used in queries from collection sample, min/max, and histogram bucket
    // boundaries.
    const samplePos = selectSamplePos(collSize, sampleSize);
    const queryValues = selectQueryValues(coll, fields, fieldTypes, samplePos, statsColl);

    // Queries are defined as documents. Example:
    // {_id: 1, pipeline: [{$match: {a: {$gt: 16}}}], "dtype": "int", "qtype" : "$gt"}.
    let testCases = generateQueries(fields, fieldTypes, queryValues);
    print(`Running ${testCases.length} queries over ${fields.length} fields.\n`);

    runQueries(coll, testCases, ceStrategies, fields, ceDebugFlag);

    // Switch to 'tryBonsai' for accuracy analysis.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    // Compute CE errors for each query and strategy and populate the error collection 'errorColl'.
    let allStrategies = [];
    for (let strategy of ceStrategies) {
        allStrategies.push(strategy);
        allStrategies.push(indexedStrategy(strategy));
    }
    populateErrorCollection(errorColl, testCases, allStrategies, collSize, false);
    assert.eq(testCases.length, errorColl.find().itcount());

    // Aggregate errors for all CE strategies per query category.
    aggregateErrorsPerCategory(errorColl, ["qtype"], allStrategies);

    // Aggregate errors for all CE strategies per data type and distribution.
    aggregateErrorsPerCategory(errorColl, ["dtype"], allStrategies);
    aggregateErrorsPerCategory(errorColl, ["distr", "dtype"], allStrategies);

    // Aggregate $elemMatch query errors per strategy.
    aggregateErrorsPerStrategy(errorColl, allStrategies, {"elemMatch": true});
    // Aggregate errors per strategy.
    aggregateErrorsPerStrategy(errorColl, allStrategies);

    printQueriesWithBadAccuracy(errorColl, testCases, "histogram", "relError");
    printQueriesWithBadAccuracy(errorColl, testCases, indexedStrategy("histogram"), "relError");
    printQueriesWithBadAccuracy(errorColl, testCases, "histogram", "absError");
    printQueriesWithBadAccuracy(errorColl, testCases, indexedStrategy("histogram"), "absError");

    jsTestLog("Complex predicates");
    let complexPred = generateComplexPredicates(testCases, fields, fieldTypes, queryValues);

    print(`Running ${complexPred.length} queries with complex predicates.\n`);
    runQueries(coll, complexPred, ceStrategies, [], ceDebugFlag);

    // Switch to 'tryBonsai' for accuracy analysis.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    const errorCollComplexPred = testDB.ce_errors_complex_pred;
    errorCollComplexPred.drop();
    populateErrorCollection(errorCollComplexPred, complexPred, allStrategies, collSize, true);
    assert.eq(complexPred.length, errorCollComplexPred.find().itcount());

    // Aggregate errors for all CE strategies per query category.
    aggregateErrorsPerCategory(errorCollComplexPred, ["qtype"], allStrategies);
    aggregateErrorsPerCategory(errorCollComplexPred, ["qtype", "numberOfTerms"], allStrategies);
    aggregateErrorsPerCategory(errorCollComplexPred, ["numberOfTerms"], allStrategies);

    aggregateErrorsPerStrategy(errorCollComplexPred, allStrategies);
}
