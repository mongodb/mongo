/**
 * Test that $minMaxScalar window function output values are as expected.
 * @tags: [featureFlagSearchHybridScoring, requires_fcv_81]
 */

const coll = db[jsTestName()];
coll.drop();

// Input documents to test against.
// The value of field "x" is what we will compute the $minMaxScalar of in these tests.
// The value of field "y" is used to test sorting docs by a field other than "_id".
let documents = [
    {_id: 1, "x": -3, "y": 8, "partition": "A"},
    {_id: 2, "x": 10, "y": 4, "partition": "B"},
    {_id: 3, "x": 3, "y": -2, "partition": "A"},
    {_id: 4, "x": 2, "y": 3, "partition": "B"},
    {_id: 5, "x": -6, "y": 6, "partition": "A"},
    {_id: 6, "x": 0, "y": -8, "partition": "B"},
    {_id: 7, "x": 4, "y": 10, "partition": "A"}
];
assert.commandWorked(coll.insertMany(documents));

// Helper function to assert the output field of $setWindowField is as expected for different input
// queries.
// Each entry in the array specifies a test case with input args to $setWindowFields,
// and the expected output field value for each document id.
// Expected test case structure:
// let testCases = [
//    {
//        setWindowFieldsArgs: {
//          ...
//        },
//        docIdToOutputFieldValue: {
//          <doc_id>: <$minMaxScalar ouput field value>,
//          ...
//        }
//    },
// ];
function validateTestCase(testCase) {
    let results = coll.aggregate([{$setWindowFields: testCase.setWindowFieldsArgs}]).toArray();
    // Check for each entry in result, the output field value is as expected
    for (let result of results) {
        assert(result.relativeXValue.toFixed(2) ==
                       testCase.docIdToOutputFieldValue[result._id].toFixed(2),
                   `'relativeXValue' of '${result.relativeXValue.toFixed(2)}' does not match 
                    expected value of '${testCase.docIdToOutputFieldValue[result._id].toFixed(2)}' 
                    for doc '_id': '${result._id}', for test with setWindowFieldsArgs = 
                   '${JSON.stringify(testCase.setWindowFieldsArgs)}'`);
    }
}
// Tests that document based bounds queries with removable winodws produce the correct output
// field value.
function testDocumentBasedRemovableQueries() {
    validateTestCase(
        // Windows that only ever include a single document should return 0 for every
        // document.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue":
                        {$minMaxScalar: {input: "$x"}, window: {documents: ["current", "current"]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 0,
                2: 0,
                3: 0,
                4: 0,
                5: 0,
                6: 0,
                7: 0,
            }
        });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScalar: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 16,
            2: 1,
            3: 9 / 10,
            4: 8 / 10,
            5: 0,
            6: 0,
            7: 0,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {documents: [-2, 2]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 9 / 16,
            4: 8 / 16,
            5: 0,
            6: 6 / 10,
            7: 1,
        }
    });
    validateTestCase(
        // Sort by 'y' instead of '_id'.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScalar: {input: "$x"},
                        window: {documents: ["current", "unbounded"]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 0,
                2: 1,
                3: 9 / 16,
                4: 8 / 16,
                5: 0,
                6: 6 / 16,
                7: 0,
            }
        });
    validateTestCase(
        // Previous case with scaled domain.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScalar: {input: "$x", min: 10000, max: 20000},
                        window: {documents: ["current", "unbounded"]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 10000,
                2: 20000,
                3: 15625,
                4: 15000,
                5: 10000,
                6: 13750,
                7: 10000,
            }
        });
    validateTestCase(
        // Testing an more complex input expression.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScalar: {input: {$add: [1, "$x"]}},
                        window: {documents: ["current", "unbounded"]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 16,
                2: 1,
                3: 9 / 10,
                4: 8 / 10,
                5: 0,
                6: 0,
                7: 0,
            }
        });
    validateTestCase(
        // Testing a constant input expression.
        // Every output value of the window should always be 0 for any constant,
        // because we only ever add in the same value into the window.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScalar: {input: {$const: 1}},
                        window: {documents: ["current", "unbounded"]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 0,
                2: 0,
                3: 0,
                4: 0,
                5: 0,
                6: 0,
                7: 0,
            }
        });
    validateTestCase(
        // Testing partitions.
        {
            setWindowFieldsArgs: {
                partitionBy: "$partition",
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {documents: [-1, 2]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 9,
                2: 1,
                3: 9 / 10,
                4: 2 / 10,
                5: 0,
                6: 0,
                7: 1,
            }
        });
}

// Tests that range based bounds queries with removable winodws produce the correct output
// field value.
function testRangeBasedRemovableQueries() {
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {range: ["current", 2]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 1,
            4: 1,
            5: 0,
            6: 0,
            7: 0,
        }
    });
    validateTestCase(
        // Sort by a field 'y' instead of '_id'.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {range: [-5, 5]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 16,
                2: 1,
                3: 1,
                4: 8 / 16,
                5: 0,
                6: 0,
                7: 1,
            }
        });
    validateTestCase(
        // Testing partitions.
        {
            setWindowFieldsArgs: {
                partitionBy: "$partition",
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {range: [-3, 4]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 10,
                2: 1,
                3: 0,
                4: 0,
                5: 0,
                6: 0,
                7: 1,
            }
        });
}

testDocumentBasedRemovableQueries();
testRangeBasedRemovableQueries();
