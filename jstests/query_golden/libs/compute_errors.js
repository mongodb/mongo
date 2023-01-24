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
    let errorDoc = {_id: testcase._id, qtype: testcase.qtype, dtype: testcase.dtype};

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
 * Given a collection containing errors for individual queries, compute root-mean square error
 * for the error documents satisfying the predicate.
 * Example predicates: {qtype: "$gt"}, {dtype: "string"}.
 */
function computeRMSE(errorColl, errorField, predicate = {}) {
    const res =
        errorColl
            .aggregate([
                {$match: predicate},
                {$project: {error2: {$pow: [errorField, 2]}}},
                {$group: {_id: null, cumError: {$sum: "$error2"}, sz: {$count: {}}}},
                {
                    $project:
                        {_id: 0, "rmse": {$round: [{$sqrt: {$divide: ["$cumError", "$sz"]}}, 3]}}
                }
            ])
            .toArray();
    if (res.length > 0) {
        return res[0].rmse;
    }
    return 0;
}

/**
 * Given a collection with a field with selectivity errors, compute the mean absolute selectivity
 * error for the error documents satisfying the predicate.
 */
function computeMeanAbsSelError(errorColl, errorField, predicate = {}) {
    const res =
        errorColl
            .aggregate([
                {$match: predicate},
                {$project: {absError: {$abs: errorField}}},
                {$group: {_id: null, cumError: {$sum: "$absError"}, sz: {$count: {}}}},
                {$project: {_id: 0, "meanErr": {$round: [{$divide: ["$cumError", "$sz"]}, 3]}}}
            ])
            .toArray();
    if (res.length > 0) {
        return res[0].meanErr;
    }
    return 0;
}

/**
 * Extract query categories in field 'predField' in the error collection.
 */
function extractCategories(errorColl, predField) {
    let categories = [];
    const res =
        errorColl.aggregate([{$group: {_id: "$" + predField}}, {$sort: {_id: 1}}]).toArray();
    res.forEach(function(doc) {
        categories.push(doc["_id"]);
    });
    return categories;
}

/**
 * Aggregate errors in the 'errorColl' for each CE strategy for each query category in 'categories'.
 */
function aggregateErrorsPerCategory(errorColl, predField, ceStrategies, aggregateAll = true) {
    jsTestLog(`CE aggregate errors per query category: ${predField}`);
    const categories = extractCategories(errorColl, predField);
    for (const op of categories) {
        const pred = {[predField]: op};
        for (const strategy of ceStrategies) {
            print(`${strategy} mean errors for ${op}: `);
            let errorField = "$" + strategy + ".absError";
            print(`absRMSE: ${computeRMSE(errorColl, errorField, pred)}, `);
            errorField = "$" + strategy + ".relError";
            print(`relRMSE: ${computeRMSE(errorColl, errorField, pred)}, `);
            errorField = "$" + strategy + ".selError";
            print(`meanAbsSelErr: ${computeMeanAbsSelError(errorColl, errorField, pred)}%\n`);
        }
    }

    if (aggregateAll == true) {
        // Compute aggregate errors across all queries.
        jsTestLog("CE aggregate errors for all queries");
        for (const strategy of ceStrategies) {
            print(`${strategy} mean errors: `);
            let errorField = "$" + strategy + ".absError";
            print(`absRMSE: ${computeRMSE(errorColl, errorField)}, `);
            errorField = "$" + strategy + ".relError";
            print(`relRMSE: ${computeRMSE(errorColl, errorField)}, `);
            errorField = "$" + strategy + ".selError";
            print(`meanAbsSelErr: ${computeMeanAbsSelError(errorColl, errorField)}%\n`);
        }
    }
}

function printQueriesWithBadAccuracy(errorColl, testCases) {
    const res = errorColl
                    .aggregate([
                        {$project: {"absSelError": {$abs: "$histogram.selError"}, "histogram": 1}},
                        {$sort: {"absSelError": -1}},
                        {$limit: 20}
                    ])
                    .toArray();

    print("Top 20 inaccurate cardinality estimates\n");
    for (const doc of res) {
        const i = doc["_id"];
        print(`Id: ${testCases[i]._id}: ${tojsononeline(testCases[i])} - Histogram errors ${
            tojsononeline(doc["histogram"])}\n`);
    }
}
