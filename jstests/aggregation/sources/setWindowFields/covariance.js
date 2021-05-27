/**
 * Test that $covariance(Pop/Samp) works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

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

// Calculate the running average of vector X and vector Y using $avg window function over the given
// 'bounds'. 'runningAvg(X/Y)' will be used to calculate covariance based on the offline algorithm -
// Cov(x, y) = ( Σ( (xi - avg(x)) * (yi - avg(y)) ) / n )
function calculateCovarianceOffline(bounds) {
    let resultOffline = coll.aggregate([
                                {
                                    $setWindowFields: {
                                        sortBy: {_id: 1},
                                        output: {
                                            runningAvgX: {$avg: "$x", window: {documents: bounds}},
                                            runningAvgY: {$avg: "$y", window: {documents: bounds}},
                                        }
                                    },
                                },
                            ])
                            .toArray();

    assert.eq(resultOffline.length, nDocs);

    // Calculate covariance based on the offline algorithm.
    for (let i = 0; i < resultOffline.length; i++) {
        // Transform the bounds to numeric indices.
        let lowerBound;
        let upperBound;
        if (bounds[0] == "unbounded")
            lowerBound = 0;
        else if (bounds[0] == "current")
            lowerBound = i;
        else
            lowerBound = Math.max(i + bounds[0], 0);

        if (bounds[1] == "unbounded")
            upperBound = resultOffline.length;
        else if (bounds[1] == "current")
            upperBound = i + 1;
        else
            upperBound = Math.min(i + bounds[1] + 1, resultOffline.length);

        let c_i = 0.0;
        let count = 0;
        for (let j = lowerBound; j < upperBound; j++, count++) {
            c_i += ((resultOffline[j].x - resultOffline[i].runningAvgX) *
                    (resultOffline[j].y - resultOffline[i].runningAvgY));
        }
        // The current window bounds are [lowerBound, upperBound);
        resultOffline[i].popCovariance = count < 1 ? null : c_i / count;
        resultOffline[i].sampCovariance = count < 2 ? null : c_i / (count - 1.0);
    }

    return resultOffline;
}

// This function compares covariance calculated based on the offline and the online algorithm to
// test the results are consistent.
// Note that the server calculates covariance based on an online algorithm -
// https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online
function compareCovarianceOfflineAndOnline(bounds) {
    let offlineRes = calculateCovarianceOffline(bounds);

    const onlineRes =
        coll.aggregate([{
                $setWindowFields: {
                    sortBy: {_id: 1},
                    output: {
                        popCovariance: {$covariancePop: ["$x", "$y"], window: {documents: bounds}},
                        sampCovariance:
                            {$covarianceSamp: ["$x", "$y"], window: {documents: bounds}},
                    }
                }
            }])
            .toArray();
    assert.eq(offlineRes.length, onlineRes.length);

    for (let i = 0; i < offlineRes.length; i++) {
        offlineRes[i].popCovariance =
            offlineRes[i].popCovariance != null ? offlineRes[i].popCovariance.toFixed(5) : null;
        offlineRes[i].sampCovariance =
            offlineRes[i].sampCovariance != null ? offlineRes[i].sampCovariance.toFixed(5) : null;
        onlineRes[i].popCovariance =
            onlineRes[i].popCovariance != null ? onlineRes[i].popCovariance.toFixed(5) : null;
        onlineRes[i].sampCovariance =
            onlineRes[i].sampCovariance != null ? onlineRes[i].sampCovariance.toFixed(5) : null;

        assert.eq(offlineRes[i].popCovariance,
                  onlineRes[i].popCovariance,
                  "Offline popCovariance: " + offlineRes[i].popCovariance +
                      " Online popCovariance: " + onlineRes[i].popCovariance);
        assert.eq(offlineRes[i].sampCovariance,
                  onlineRes[i].sampCovariance,
                  "Offline sampCovariance: " + offlineRes[i].sampCovariance +
                      " Online sampCovariance: " + onlineRes[i].sampCovariance);
    }
}

// Test various type of window.
documentBounds.forEach(compareCovarianceOfflineAndOnline);
})();
