/**
 * Compute cardinality estimation errors for a testcase and CE strategy.
 * Example testcase:
 * { _id: 2, pipeline: [...], nReturned: 2, "heuristic": 4.47, "histogram": 2, ...}
 * Returns : {"absError": 2.47, "relError": 1.23, "selError": 12.35}
 */
function computeStrategyErrors(testcase, strategy, collSize) {
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
    return {
        "absError": round2(absError),
        "relError": round2(relError),
        "selError": round2(selError)
    };
}

/**
 * Compute cardinality estimation errors for a testcase for all CE strategies.
 */
function computeAndPrintErrors(testcase, ceStrategies, collSize) {
    let errorDoc = {
        _id: testcase._id,
        qtype: testcase.qtype,
        dtype: testcase.dtype,
        fieldName: testcase.fieldName,
        elemMatch: testcase.elemMatch
    };

    ceStrategies.forEach(function(strategy) {
        const errors = computeStrategyErrors(testcase, strategy, collSize);
        errorDoc[strategy] = errors;
        print(`${strategy}: ${testcase[strategy]} `);
        print(`AbsError: ${errors["absError"]}, RelError: ${errors["relError"]}, SelError: ${
            errors["selError"]}%\n`);
    });
    return errorDoc;
}

/**
 * Compute CE errors for each query and populate the error collection 'errorColl'.
 */
function populateErrorCollection(errorColl, testCases, ceStrategies, collSize) {
    for (const testcase of testCases) {
        jsTestLog(`Query ${testcase._id}: ${tojsononeline(testcase.pipeline)}`);
        print(`Actual cardinality: ${testcase.nReturned}\n`);
        print(`Cardinality estimates:\n`);
        const errorDoc = computeAndPrintErrors(testcase, ceStrategies, collSize);
        assert.commandWorked(errorColl.insert(errorDoc));
    }
}

/**
 * Aggregate errors in the 'errorColl' on the 'groupField' for each CE strategy.
 */
function aggregateErrorsPerCategory(errorColl, groupField, ceStrategies) {
    jsTestLog(`Mean errors per ${groupField}:`);
    for (const strategy of ceStrategies) {
        const absError = "$" + strategy + ".absError";
        const relError = "$" + strategy + ".relError";
        const selError = "$" + strategy + ".selError";
        const res =
            errorColl
                .aggregate([
                    {
                        $project: {
                            category: "$" + groupField,
                            absError2: {$pow: [absError, 2]},
                            relError2: {$pow: [relError, 2]},
                            absSelError: {$abs: selError}
                        }
                    },
                    {
                        $group: {
                            _id: "$category",
                            cumAbsError2: {$sum: "$absError2"},
                            cumRelError2: {$sum: "$relError2"},
                            cumSelError: {$sum: "$absSelError"},
                            sz: {$count: {}}
                        }
                    },
                    {
                        $project: {
                            "category": "$_id",
                            "queryCount": "$sz",
                            "_id": 0,
                            "RMSAbsError":
                                {$round: [{$sqrt: {$divide: ["$cumAbsError2", "$sz"]}}, 3]},
                            "RMSRelError":
                                {$round: [{$sqrt: {$divide: ["$cumRelError2", "$sz"]}}, 3]},
                            "meanSelError": {$round: [{$divide: ["$cumSelError", "$sz"]}, 3]}
                        }
                    },
                    {$sort: {"category": 1}}
                ])
                .toArray();

        print(`${strategy}:\n`);
        for (const doc of res) {
            print(`${tojsononeline(doc)}\n`);
        }
    }
}

/**
 * Aggregate errors in the 'errorColl' per CE strategy. If a predicate is provided
 * aggregate only the error documents which satisfy the predicate.
 */
function aggregateErrorsPerStrategy(errorColl, ceStrategies, predicate = {}) {
    jsTestLog(`Mean errors per strategy for predicate ${tojsononeline(predicate)}:`);
    for (const strategy of ceStrategies) {
        const absError = "$" + strategy + ".absError";
        const relError = "$" + strategy + ".relError";
        const selError = "$" + strategy + ".selError";
        const res =
            errorColl
                .aggregate([
                    {$match: predicate},
                    {
                        $project: {
                            absError2: {$pow: [absError, 2]},
                            relError2: {$pow: [relError, 2]},
                            absSelError: {$abs: selError}
                        }
                    },
                    {
                        $group: {
                            _id: null,
                            cumAbsError2: {$sum: "$absError2"},
                            cumRelError2: {$sum: "$relError2"},
                            cumSelError: {$sum: "$absSelError"},
                            sz: {$count: {}}
                        }
                    },
                    {
                        $project: {
                            _id: 0,
                            "RMSAbsError":
                                {$round: [{$sqrt: {$divide: ["$cumAbsError2", "$sz"]}}, 3]},
                            "RMSRelError":
                                {$round: [{$sqrt: {$divide: ["$cumRelError2", "$sz"]}}, 3]},
                            "meanSelError": {$round: [{$divide: ["$cumSelError", "$sz"]}, 3]}
                        }
                    }
                ])
                .toArray();

        print(`${strategy}: `);
        for (const doc of res) {
            print(`${tojsononeline(doc)}\n`);
        }
    }
}

/**
 * Aggregate errors in the 'errorColl' for each CE strategy per data distribution.
 * The function groups fieldNames with common prefix ending with a "_". In the CE tests we use this
 * prefix to encode the type of data distribution 'uniform_int_100', 'normal_int_1000'.
 */
function aggregateErrorsPerDataDistribution(errorColl, ceStrategies) {
    jsTestLog('Mean errors per data distribution:');
    for (const strategy of ceStrategies) {
        const absError = "$" + strategy + ".absError";
        const relError = "$" + strategy + ".relError";
        const selError = "$" + strategy + ".selError";
        const res =
            errorColl
                .aggregate([
                    {
                        $project: {
                            "distr":
                                {$substr: ["$fieldName", 0, {$indexOfCP: ["$fieldName", "_"]}]},
                            absError2: {$pow: [absError, 2]},
                            relError2: {$pow: [relError, 2]},
                            absSelError: {$abs: selError}
                        }
                    },
                    {
                        $group: {
                            _id: "$distr",
                            cumAbsError2: {$sum: "$absError2"},
                            cumRelError2: {$sum: "$relError2"},
                            cumSelError: {$sum: "$absSelError"},
                            sz: {$count: {}}
                        }
                    },
                    {
                        $project: {
                            "RMSAbsErr":
                                {$round: [{$sqrt: {$divide: ["$cumAbsError2", "$sz"]}}, 3]},
                            "RMSRelErr":
                                {$round: [{$sqrt: {$divide: ["$cumRelError2", "$sz"]}}, 3]},
                            "meanSelErr": {$round: [{$divide: ["$cumSelError", "$sz"]}, 3]}
                        }
                    },
                    {$sort: {_id: 1}}
                ])
                .toArray();

        print(`${strategy}:\n`);
        for (const doc of res) {
            print(`${tojsononeline(doc)}\n`);
        }
    }
}

/**
 * Find top 10 inacurate estimates for a strategy and an error field.
 */
function printQueriesWithBadAccuracy(errorColl, testCases, strategy, errorField, count = 10) {
    const errorFieldName = strategy + "." + errorField;
    const res = errorColl
                    .aggregate([
                        {$match: {[errorFieldName]: {$gt: 0.001}}},
                        {$project: {"absError": {$abs: "$" + errorFieldName}, [strategy]: 1}},
                        {$sort: {"absError": -1}},
                        {$limit: 10}
                    ])
                    .toArray();

    jsTestLog(`Top ${count} inaccurate cardinality estimates by ${strategy} according to the ${
        errorField} field:`);
    for (const doc of res) {
        const i = doc["_id"];
        const test = testCases[i];
        print(`Id: ${test._id}: ${tojsononeline(test.pipeline)}, qtype: ${test.qtype}, data type: ${test.dtype}, 
cardinality: ${test.nReturned}, Histogram estimation: ${test[strategy]}, errors: ${tojsononeline(doc[strategy])}\n`);
    }
}
