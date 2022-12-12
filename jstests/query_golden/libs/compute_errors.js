// Given a collection containing errors for individual queries, compute root-mean square error.
function computeRMSE(errorColl, errorField, collSize) {
    const res =
        errorColl
            .aggregate([
                {$project: {error2: {$pow: [errorField, 2]}}},
                {$group: {_id: null, cumError: {$sum: "$error2"}}},
                {
                    $project:
                        {_id: 0, "rmse": {$round: [{$sqrt: {$divide: ["$cumError", collSize]}}, 3]}}
                }
            ])
            .toArray();
    return res[0].rmse;
}

// Given a collection with a field with selectivity errors, compute the mean absolute selectivity
// error.
function computeMeanAbsSelError(errorColl, errorField, collSize) {
    const res =
        errorColl
            .aggregate([
                {$project: {absError: {$abs: errorField}}},
                {$group: {_id: null, cumError: {$sum: "$absError"}}},
                {$project: {_id: 0, "meanErr": {$round: [{$divide: ["$cumError", collSize]}, 3]}}}
            ])
            .toArray();
    return res[0].meanErr;
}
