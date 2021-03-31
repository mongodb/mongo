/**
 * Test that exponential moving average works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

const origDocs = coll.find().sort({_id: 1}).toArray();

// startIndex inclusive, endIndex exclusive.
function movingAvgForDocs(alpha) {
    let results = [];
    let lastVal = null;
    for (let i = 0; i < origDocs.length; i++) {
        if (!lastVal) {
            lastVal = origDocs[i].price;
        } else {
            lastVal = origDocs[i].price * alpha + lastVal * (1 - alpha);
        }
        results.push(lastVal);
    }
    return results;
}

// Test that $expMovingAvg returns null for windows which do not contain numeric values.
let results = coll.aggregate([
                      {$addFields: {str: "hiya"}},
                      {
                          $setWindowFields: {
                              sortBy: {_id: 1},
                              output: {
                                  expMovAvg: {$expMovingAvg: {input: "$str", N: 2}},
                              }
                          }
                      }
                  ])
                  .toArray();
for (let index = 0; index < results.length; index++) {
    assert.eq(null, results[index].expMovAvg);
}
// Test simple case with N specified.
results = coll.aggregate([
                  {
                      $setWindowFields: {
                          sortBy: {_id: 1},
                          output: {
                              expMovAvg: {
                                  $expMovingAvg: {input: "$price", N: 3},
                              },
                          }
                      }
                  },
                  // Working with NumberDecimals in JS is difficult. Compare doubles instead.
                  {$addFields: {doubleVal: {$toDouble: "$expMovAvg"}}}
              ])
              .toArray();
const simpleNResult = movingAvgForDocs(2.0 / (3.0 + 1));

// Same test with manual alpha
for (let index = 0; index < results.length; index++) {
    assert.close(simpleNResult[index], results[index].doubleVal, "Disagreement at index " + index);
}

results = coll.aggregate([
                  {
                      $setWindowFields: {
                          sortBy: {_id: 1},
                          output: {
                              expMovAvg: {
                                  $expMovingAvg: {input: "$price", alpha: .5},
                              },
                          }
                      }
                  },
                  // Working with NumberDecimals in JS is difficult. Compare doubles instead.
                  {$addFields: {doubleVal: {$toDouble: "$expMovAvg"}}}
              ])
              .toArray();
const simpleAlphaResult = movingAvgForDocs(.5);
for (let index = 0; index < results.length; index++) {
    assert.close(
        simpleAlphaResult[index], results[index].doubleVal, "Disagreement at index " + index);
}

// Succeed with more interesting alpha.
results = coll.aggregate([
                  {
                      $setWindowFields: {
                          sortBy: {_id: 1},
                          output: {
                              expMovAvg: {
                                  $expMovingAvg: {input: "$price", alpha: .279},
                              },
                          }
                      }
                  },
                  // Working with NumberDecimals in JS is difficult. Compare doubles instead.
                  {$addFields: {doubleVal: {$toDouble: "$expMovAvg"}}}
              ])
              .toArray();
const complexAlphaResult = movingAvgForDocs(.279);
for (let index = 0; index < results.length; index++) {
    assert.close(
        complexAlphaResult[index], results[index].doubleVal, "Disagreement at index " + index);
}

// Fails if argument type or contents are incorrect.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: {input: "$price", alpha: .5, N: 2},
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: {input: "$price", N: .5},
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: {input: "$price", N: "food"},
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: {str: "$price", N: 2},
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: {str: "$price", N: 2},
                        randomArg: 2,
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    expMovAvg: {
                        $expMovingAvg: "$price",
                    },
                }
            }
        },
    ]
}),
                             ErrorCodes.FailedToParse);
})();
