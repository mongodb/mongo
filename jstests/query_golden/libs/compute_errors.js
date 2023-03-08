/**
 * Compute cardinality estimation errors for a testcase and CE strategy.
 * Example testcase:
 * { _id: 2, pipeline: [...], nReturned: 2, "heuristic": 4.47, "histogram": 2, ...}
 * Returns : {"qError": 2.23, "relError": 1.23, "selError": 12.35}
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

    const min = Math.min(testcase[strategy], testcase.nReturned);
    const max = Math.max(testcase[strategy], testcase.nReturned);
    const qError = (min > 0) ? max / min : max;
    // Selectivity error wrt collection size.
    const selError = 100.0 * absError / collSize;
    return {"qError": round2(qError), "relError": round2(relError), "selError": round2(selError)};
}

/**
 * Compute cardinality estimation errors for a testcase for all CE strategies.
 */
function computeAndPrintErrors(testcase, ceStrategies, collSize, isComplex) {
    let errorDoc = {_id: testcase._id, qtype: testcase.qtype};
    if (isComplex == true) {
        errorDoc["numberOfTerms"] = testcase.numberOfTerms;
    } else {
        const categories = testcase.fieldName.split("_");
        errorDoc["dtype"] = testcase.dtype;
        errorDoc["distr"] = categories[0];
        errorDoc["fieldName"] = testcase.fieldName;
        errorDoc["elemMatch"] = testcase.elemMatch;
    }
    assert(testcase.nReturned >= 0, `testcase: ${tojson(testcase)}`);
    errorDoc["nReturned"] = testcase.nReturned;

    ceStrategies.forEach(function(strategy) {
        const errors = computeStrategyErrors(testcase, strategy, collSize);
        errorDoc[strategy] = errors;
        print(`${strategy}: ${testcase[strategy]} `);
        print(`QError: ${errors["qError"]}, RelError: ${errors["relError"]}, SelError: ${
            errors["selError"]}%\n`);
        duration = 'duration_' + strategy;
        errorDoc[duration] = testcase[duration];
    });
    return errorDoc;
}

/**
 * Compute CE errors for each query and populate the error collection 'errorColl'.
 */
function populateErrorCollection(errorColl, testCases, ceStrategies, collSize, isComplex) {
    for (const testcase of testCases) {
        jsTestLog(`Query ${testcase._id}: ${tojsononeline(testcase.pipeline)}`);
        print(`Actual cardinality: ${testcase.nReturned}\n`);
        print(`Cardinality estimates:\n`);
        const errorDoc = computeAndPrintErrors(testcase, ceStrategies, collSize, isComplex);
        assert.commandWorked(errorColl.insert(errorDoc));
    }
}

/**
 * Given an array of fields on which we want to perform $group, return an expression computing the
 * group key.
 */
function makeGroupKey(groupFields) {
    let args = [];
    for (let i = 0; i < groupFields.length; i++) {
        args.push({$toString: "$" + groupFields[i]});
        if (i < groupFields.length - 1) {
            args.push("_");
        }
    }
    return {$concat: args};
}

/**
 * Aggregate errors in the 'errorColl' on the 'groupFields' for each CE strategy.
 */
function aggregateErrorsPerCategory(errorColl, groupFields, ceStrategies) {
    const groupKey = makeGroupKey(groupFields);
    jsTestLog(`Mean errors per ${tojsononeline(groupFields)}:`);
    for (const strategy of ceStrategies) {
        const qError = "$" + strategy + ".qError";
        const relError = "$" + strategy + ".relError";
        const selError = "$" + strategy + ".selError";
        const res =
            errorColl
                .aggregate([
                    {
                        $project: {
                            category: groupKey,
                            qError2: {$pow: [qError, 2]},
                            relError2: {$pow: [relError, 2]},
                            absSelError: {$abs: selError}
                        }
                    },
                    {
                        $group: {
                            _id: "$category",
                            cumQError2: {$sum: "$qError2"},
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
                            "RMSQError": {$round: [{$sqrt: {$divide: ["$cumQError2", "$sz"]}}, 3]},
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
    const msg = (Object.keys(predicate).length == 0) ? "all queries"
                                                     : `predicate ${tojsononeline(predicate)}:`;
    jsTestLog(`Mean errors per strategy for ${msg}:`);

    for (const strategy of ceStrategies) {
        const qError = "$" + strategy + ".qError";
        const relError = "$" + strategy + ".relError";
        const selError = "$" + strategy + ".selError";
        const res =
            errorColl
                .aggregate([
                    {$match: predicate},
                    {
                        $project: {
                            qError2: {$pow: [qError, 2]},
                            relError2: {$pow: [relError, 2]},
                            absSelError: {$abs: selError}
                        }
                    },
                    {
                        $group: {
                            _id: null,
                            cumQError2: {$sum: "$qError2"},
                            cumRelError2: {$sum: "$relError2"},
                            cumSelError: {$sum: "$absSelError"},
                            sz: {$count: {}}
                        }
                    },
                    {
                        $project: {
                            _id: 0,
                            "RMSQError": {$round: [{$sqrt: {$divide: ["$cumQError2", "$sz"]}}, 3]},
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

function aggegateOptimizationTimesPerStrategy(errorColl, ceStrategies) {
    print("Average optimization time per strategy:");
    for (const strategy of ceStrategies) {
        const strategyDuration = "$" +
            "duration_" + strategy;
        const res =
            errorColl
                .aggregate([
                    {$project: {dur: strategyDuration}},
                    {$group: {_id: null, durationSum: {$sum: "$dur"}, sz: {$count: {}}}},
                    {
                        $project: {
                            _id: 0,
                            "totalDuration": "$durationSum",
                            "averageDuration": {$round: [{$divide: ["$durationSum", "$sz"]}, 3]},
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
cardinality: ${test.nReturned}, ${strategy} estimation: ${test[strategy]}, errors: ${tojsononeline(doc[strategy])}\n`);
    }
}
