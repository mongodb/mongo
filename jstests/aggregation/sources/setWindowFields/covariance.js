/**
 * Test that $covariance(Pop/Samp) works as a window function.
 * Currently only tests accumulator-type window function.
 */
(function() {
"use strict";

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

const nonRemovableCovStage = {
    $setWindowFields: {
        sortBy: {_id: 1},
        output: {
            popCovariance:
                {$covariancePop: ["$x", "$y"], window: {documents: ["unbounded", "current"]}},
            sampCovariance:
                {$covarianceSamp: ["$x", "$y"], window: {documents: ["unbounded", "current"]}},
        }
    },
};

// Basic tests.
assert.commandWorked(coll.insert({_id: 1, x: 0, y: 0}));
assert.commandWorked(coll.insert({_id: 2, x: 2, y: 2}));

const result = coll.aggregate([nonRemovableCovStage]).toArray();
assert.eq(result.length, 2);
assert.eq(result[0].popCovariance.toFixed(2), 0.00);
assert.eq(result[0].sampCovariance, null);
assert.eq(result[1].popCovariance.toFixed(2), 1.00);
assert.eq(result[1].sampCovariance.toFixed(2), 2.00);

coll.drop();
const nDocs = 10;
for (let i = 1; i <= nDocs; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        x: Math.random(),
        y: Math.random(),
    }));
}

// Caculate the running average of vector X and vector Y using $avg window function. The running
// average of each document is the current average of 'X' and 'Y' in window [unbounded, current].
// 'runningAvg(X/Y)' will be used to calculate covariance based on the offline algorithm -
// Cov(x, y) = ( Σ( (xi - avg(x)) * (yi - avg(y)) ) / n )
function calculateCovarianceOffline() {
    let resultOffline =
        coll.aggregate([
                {
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {
                            runningAvgX:
                                {$avg: "$x", window: {documents: ["unbounded", "current"]}},
                            runningAvgY:
                                {$avg: "$y", window: {documents: ["unbounded", "current"]}},
                        }
                    },
                },
            ])
            .toArray();

    assert.eq(resultOffline.length, nDocs);
    resultOffline[0].popCovariance = 0.0;
    resultOffline[0].sampCovariance = null;

    for (let i = 1; i < resultOffline.length; i++) {
        let c_i = 0.0;
        for (let j = 0; j <= i; j++) {
            c_i += ((resultOffline[j].x - resultOffline[i].runningAvgX) *
                    (resultOffline[j].y - resultOffline[i].runningAvgY));
        }
        resultOffline[i].popCovariance = c_i / (i + 1);
        resultOffline[i].sampCovariance = c_i / i;
    }

    return resultOffline;
}

// This function compares covariance calculated based on the offline and the online algorithm to
// test the results are consistent.
// Note that the server calculates covariance based on an online algorithm -
// https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online
(function compareCovarianceOfflineAndOnline() {
    const offlineRes = calculateCovarianceOffline();
    const onlineRes = coll.aggregate([nonRemovableCovStage]).toArray();

    assert.eq(offlineRes.length, onlineRes.length);
    assert.eq(onlineRes.length, nDocs);
    assert.eq(onlineRes[0].popCovariance, 0.0);
    assert.eq(onlineRes[0].sampCovariance, null);
    for (let i = 1; i < offlineRes.length; i++) {
        assert.eq(offlineRes[i].popCovariance.toFixed(5), onlineRes[i].popCovariance.toFixed(5));
        assert.eq(offlineRes[i].sampCovariance.toFixed(5), onlineRes[i].sampCovariance.toFixed(5));
    }
})();
})();
