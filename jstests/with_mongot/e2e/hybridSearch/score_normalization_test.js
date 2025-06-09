/**
 * Tests that the $score normalization works as expected.
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

const test_field_name = "counter";
const dollar_test_field_name = "$" + test_field_name;

const coll = db[jsTestName()];
coll.drop();
const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10;

for (let i = 1; i <= nDocs; i++) {
    bulk.insert({i, [test_field_name]: i});
}
assert.commandWorked(bulk.execute());

// Test that basic $score with no normalization works.
(function testBasicScoreWithNoNormalization() {
    const actualResults = coll.aggregate([
                                  {$score: {score: dollar_test_field_name, normalization: "none"}},
                                  {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                                  {$sort: {_id: 1}}
                              ])
                              .toArray();

    const expectedResults = coll.aggregate([
                                    {$project: {_id: 1, counter: 1, score: dollar_test_field_name}},
                                    {$sort: {_id: 1}}
                                ])
                                .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that an expression $score with no normalization works.
(function testExpressionScoreWithNoNormalization() {
    const actualResults =
        coll.aggregate([
                {$score: {score: {$multiply: [dollar_test_field_name, 2]}, normalization: "none"}},
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, counter: 1, score: {$multiply: [dollar_test_field_name, 2]}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score with a weight with no normalization works.
(function testWeightedScoreWithNoNormalization() {
    const actualResults =
        coll.aggregate([
                {$score: {score: dollar_test_field_name, normalization: "none", weight: 0.5}},
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, counter: 1, score: {$multiply: [dollar_test_field_name, 0.5]}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score, as an expression and with a weight, with no normalization works.
(function testWeightedExpressionScoreWithNoNormalization() {
    // By squaring the input, we achieve a different distribution amongst the scores than just
    // incrementing (linear distribution).
    const actualResults =
        coll.aggregate([
                {
                    $score: {
                        score: {$multiply: [dollar_test_field_name, dollar_test_field_name]},
                        normalization: "none",
                        weight: 0.5
                    }
                },
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        counter: 1,
                        score: {$multiply: [dollar_test_field_name, dollar_test_field_name, 0.5]}
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that basic $score with sigmoid normalization works.
(function testBasicScoreWithSigmoidNormalization() {
    const actualResults =
        coll.aggregate([
                {$score: {score: dollar_test_field_name, normalization: "sigmoid"}},
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, counter: 1, score: {$sigmoid: dollar_test_field_name}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that an expression $score with sigmoid normalization works.
(function testExpressionScoreWithSigmoidNormalization() {
    const actualResults =
        coll.aggregate([
                {
                    $score:
                        {score: {$multiply: [dollar_test_field_name, 2]}, normalization: "sigmoid"}
                },
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        counter: 1,
                        score: {$sigmoid: {$multiply: [dollar_test_field_name, 2]}}
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score with a weight with sigmoid normalization works.
(function testWeightedScoreWithSigmoidNormalization() {
    const actualResults =
        coll.aggregate([
                {$score: {score: dollar_test_field_name, normalization: "sigmoid", weight: 0.3}},
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        counter: 1,
                        score: {$multiply: [0.3, {$sigmoid: dollar_test_field_name}]}
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score, as an expression and with a weight, with sigmoid normalization works.
(function testWeightedExpressionScoreWithSigmoidNormalization() {
    const actualResults =
        coll.aggregate([
                {
                    $score: {
                        score: {$multiply: [dollar_test_field_name, dollar_test_field_name]},
                        normalization: "sigmoid",
                        weight: 0.3
                    }
                },
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        counter: 1,
                        score: {
                            $multiply: [
                                0.3,
                                {
                                    $sigmoid: {
                                        $multiply: [dollar_test_field_name, dollar_test_field_name]
                                    }
                                }
                            ]
                        }
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that basic $score with minMaxScaler normalization works.
(function testBasicScoreWithMinMaxScalerNormalization() {
    const actualResults =
        coll.aggregate([
                {$score: {score: dollar_test_field_name, normalization: "minMaxScaler"}},
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            score: {
                                $minMaxScaler: {input: dollar_test_field_name},
                                window: {documents: ["unbounded", "unbounded"]}
                            }
                        }
                    }
                },
                {$project: {_id: 1, counter: 1, score: 1}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that an expression $score with minMaxScaler normalization works.
(function testExpressionScoreWithMinMaxScalerNormalization() {
    const actualResults = coll.aggregate([
                                  {
                                      $score: {
                                          score: {$multiply: [dollar_test_field_name, 2]},
                                          normalization: "minMaxScaler"
                                      }
                                  },
                                  {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                                  {$sort: {_id: 1}}
                              ])
                              .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            score: {
                                $minMaxScaler: {input: {$multiply: [dollar_test_field_name, 2]}},
                                window: {documents: ["unbounded", "unbounded"]}
                            }
                        }
                    }
                },
                {$project: {_id: 1, counter: 1, score: 1}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score with a weight with minMaxScaler normalization works.
(function testWeightedScoreWithMinMaxScalerNormalization() {
    const actualResults =
        coll.aggregate([
                {
                    $score:
                        {score: dollar_test_field_name, normalization: "minMaxScaler", weight: 0.75}
                },
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            score: {
                                $minMaxScaler: {input: dollar_test_field_name},
                                window: {documents: ["unbounded", "unbounded"]}
                            }
                        }
                    }
                },
                {$project: {_id: 1, counter: 1, score: {$multiply: ["$score", 0.75]}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();

// Test that $score, as an expression and with a weight, with minMaxScaler normalization works.
(function testWeightedExpressionScoreWithMinMaxScalerNormalization() {
    const actualResults =
        coll.aggregate([
                {
                    $score: {
                        score: {$multiply: [dollar_test_field_name, dollar_test_field_name]},
                        normalization: "minMaxScaler",
                        weight: 0.75
                    }
                },
                {$project: {_id: 1, counter: 1, score: {$meta: "score"}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            score: {
                                $minMaxScaler: {
                                    input: {
                                        $multiply: [dollar_test_field_name, dollar_test_field_name]
                                    }
                                },
                                window: {documents: ["unbounded", "unbounded"]}
                            }
                        }
                    }
                },
                {$project: {_id: 1, counter: 1, score: {$multiply: ["$score", 0.75]}}},
                {$sort: {_id: 1}}
            ])
            .toArray();

    assert.eq(actualResults, expectedResults);
})();
