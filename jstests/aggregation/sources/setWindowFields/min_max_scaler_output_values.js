/**
 * Test that $minMaxScaler window function output values are as expected.
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

const coll = db[jsTestName()];
coll.drop();

// Input documents to test against.
// The value of field "x" is what we will compute the $minMaxScaler of in these tests.
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
//          <doc_id>: <$minMaxScaler output field value>,
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

// Document id to field value map (where input is '$x')
// for special case where the window is left and right unbounded.
// In these cases where both the left and right are unbounded, the window never slides,
// so all (non-partitioned) $minMaxScaler queries result in this expected map.
// This implies that for these ['unbounded', 'unbounded'] windows it does not matter
// if the windowing scheme is 'documents' or 'range', or what the 'sortBy' key is.
const expectedDocIdToOutputFieldValueForUnboundedQueries = {
    1: 3 / 16,
    2: 1,
    3: 9 / 16,
    4: 8 / 16,
    5: 0,
    6: 6 / 16,
    7: 10 / 16,
};

// Tests that document based bounds queries with non-removable windows produce the correct output
// field value. Non-removable queries are those which are unbounded on the left.
function testDocumentBasedNonRemovableQueries() {
    // Testing left and right unbounded windows.
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Same as last case, but not specifying bounds (which default to the same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScaler: {input: "$x"}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Tests reverse sorting in unbounded case. In an unbounded case, the results should not depend
    // on sorting field (so results are same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Tests reverse sorting in unbounded case. In an unbounded case, the results should not depend
    // on sorting field (so results are same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Left and right unbounded query should not depend on sorting field
    // (so results are same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Testing right non-unbounded windows.
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "current"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 6 / 13,
            4: 5 / 13,
            5: 0,
            6: 6 / 16,
            7: 10 / 16,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 6 / 13,
            4: 8 / 16,
            5: 0,
            6: 6 / 16,
            7: 10 / 16,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 16,  // includes all
            2: 1,       // includes all
            3: 9 / 16,  // includes [4, 0, -6, 2, 3, 10]
            4: 8 / 10,  // includes [4, 0, -6, 2, 3]
            5: 0,       // includes [4, 0, -6, 2]
            6: 6 / 10,  // includes [4, 0, -6]
            7: 1,       // includes [4, 0]
        }
    });
    // List of documents with y sorted descending:
    // {_id: 7, "x": 4, "y": 10, "partition": "A"},
    // {_id: 1, "x": -3, "y": 8, "partition": "A"},
    // {_id: 5, "x": -6, "y": 6, "partition": "A"},
    // {_id: 2, "x": 10, "y": 4, "partition": "B"},
    // {_id: 4, "x": 2, "y": 3, "partition": "B"},
    // {_id: 3, "x": 3, "y": -2, "partition": "A"},
    // {_id: 6, "x": 0, "y": -8, "partition": "B"},
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 10,  // includes [4, -3, -6]
            2: 1,       // includes [4, -3, -6, 10, 2]
            3: 9 / 16,  // includes all
            4: 1 / 2,   // includes [4, -3, -6, 10, 2, 3]
            5: 0,       // includes [4, -3, -6, 10]
            6: 3 / 8,   // includes all
            7: 1,       // includes [4, -3]
        }
    });
    validateTestCase(
        // Previous case with scaled domain.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: "$x", min: 10000, max: 20000},
                        window: {documents: ["unbounded", 1]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 10000,
                2: 20000,
                3: 190000 / 13,
                4: 15000,
                5: 10000,
                6: 13750,
                7: 16250,
            }
        });

    validateTestCase({
        // Sort by a field 'y' instead of '_id'.
        setWindowFieldsArgs: {
            sortBy: {y: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "current"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 16,
            2: 1,
            3: 1,
            4: 2 / 3,
            5: 0,
            6: 0,
            7: 10 / 16,
        }
    });
    validateTestCase({
        // Sort by a field 'y' instead of '_id'.
        setWindowFieldsArgs: {
            sortBy: {y: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 16,
            2: 1,
            3: 1,
            4: 2 / 10,
            5: 0,
            6: 0,
            7: 10 / 16,
        }
    });
    // Testing other / more complex input expressions
    validateTestCase(
        // Testing a constant input expression.
        // Every output value of the window should always be 0 for any constant,
        // because we only ever add in the same value into the window.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: {$const: 1}},
                        window: {documents: ["unbounded", 1]}
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
        // Testing a more complex input expression.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: {$add: ["$x", "$y"]}},
                        window: {documents: ["unbounded", 2]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 4 / 13,
                2: 1,
                3: 1 / 14,
                4: 13 / 22,
                5: 8 / 22,
                6: 0,
                7: 1,
            }
        });
    // Testing partitions
    validateTestCase({
        setWindowFieldsArgs: {
            partitionBy: "$partition",
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 10,
            2: 1,
            3: 9 / 10,
            4: 2 / 10,
            5: 0,
            6: 0,
            7: 1,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            partitionBy: "$partition",
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 1,
            4: 2 / 10,
            5: 0,
            6: 0,
            7: 1,
        }
    });
}

// Tests that range based bounds queries with non-removable windows produce the correct output
// field value. Non-removable queries are those which are unbounded on the left.
function testRangeBasedNonRemovableQueries() {
    // Testing left and right unbounded windows.
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Left and right unbounded ranges should not depend on sorting field
    // (so results are same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Testing right non-unbounded windows.
    // Range based window sorted by '_id' is equivalent to document bounds sorted by '_id'
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "current"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 6 / 13,
            4: 5 / 13,
            5: 0,
            6: 6 / 16,
            7: 10 / 16,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", 1]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,
            2: 1,
            3: 6 / 13,
            4: 8 / 16,
            5: 0,
            6: 6 / 16,
            7: 10 / 16,
        }
    });
    // Testing left and right unbounded windows.
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    // Left and right unbounded ranges should not depend on sorting field
    // (so results are same as above).
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: expectedDocIdToOutputFieldValueForUnboundedQueries
    });
    validateTestCase(
        // Previous case with scaled domain.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: "$x", min: 10000, max: 20000},
                        window: {range: ["unbounded", "unbounded"]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 11875,
                2: 20000,
                3: 15625,
                4: 15000,
                5: 10000,
                6: 13750,
                7: 16250,
            }
        });
    validateTestCase(
        // Sort by a field 'y' instead of '_id'.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "current"]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 16,
                2: 1,
                3: 1,
                4: 2 / 3,
                5: 0,
                6: 0,
                7: 10 / 16,
            }
        });
    validateTestCase(
        // Sort by a field 'y' instead of '_id'.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", 5]}},
                }
            },
            docIdToOutputFieldValue: {
                1: 3 / 16,
                2: 1,
                3: 1,
                4: 8 / 16,
                5: 0,
                6: 0,
                7: 10 / 16,
            }
        });
    // Testing other / more complex input expressions
    validateTestCase(
        // Testing a constant input expression.
        // Every output value of the window should always be 0 for any constant,
        // because we only ever add in the same value into the window.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: {$const: 1}}, window: {range: ["unbounded", 1]}},
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
        // Testing a more complex input expression.
        {
            setWindowFieldsArgs: {
                sortBy: {y: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: {$add: ["$x", "$y"]}},
                        window: {range: ["unbounded", 5]}
                    },
                }
            },
            docIdToOutputFieldValue: {
                1: 13 / 22,
                2: 1,
                3: 9 / 13,
                4: 13 / 22,
                5: 8 / 22,
                6: 0,
                7: 1,
            }
        });
    // Testing partitions
    validateTestCase({
        setWindowFieldsArgs: {
            partitionBy: "$partition",
            sortBy: {_id: 1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 10,
            2: 1,
            3: 9 / 10,
            4: 2 / 10,
            5: 0,
            6: 0,
            7: 1,
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            partitionBy: "$partition",
            sortBy: {y: 1},
            output: {
                "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", 2]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 10,
            2: 1,
            3: 0,
            4: 2 / 10,
            5: 0,
            6: 0,
            7: 1,
        }
    });
}

// Tests that document based bounds queries with removable windows produce the correct output
// field value. Removable queries are those which are bounded on the left
// (some values are removed from the window).
function testDocumentBasedRemovableQueries() {
    validateTestCase(
        // Windows that only ever include a single document should return 0 for every
        // document.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "current"]}},
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
        // Windows that only ever include a single document should return 0 for every
        // document.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: -1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "current"]}},
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
        // Windows that only ever include a single document should return 0 for every
        // document.
        {
            setWindowFieldsArgs: {
                sortBy: {y: -1},
                output: {
                    "relativeXValue":
                        {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "current"]}},
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
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
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
            sortBy: {_id: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,       // includes [-3]
            2: 1,       // includes [10, -3]
            3: 6 / 13,  // includes [3, 10, -3]
            4: 5 / 13,  // includes [2, 3, 10, -3]
            5: 0,       // includes [-6, 2, 3, 10, -3]
            6: 3 / 8,   // includes [0, -6, 2, 3, 10, -3]
            7: 5 / 8,   // includes [4, 0, -6, 2, 3, 10, -3]
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: [-1, "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 0,       // includes [10, -3]
            2: 1,       // includes [3, 10, -3]
            3: 6 / 13,  // includes [2, 3, 10, -3]
            4: 1 / 2,   // includes [-6, 2, 3, 10, -3]
            5: 0,       // includes [0, -6, 2, 3, 10, -3]
            6: 3 / 8,   // includes [4, 0, -6, 2, 3, 10, -3]
            7: 5 / 8,   // includes [4, 0, -6, 2, 3, 10, -3]
        }
    });
    // List of documents with y sorted descending:
    // {_id: 7, "x": 4, "y": 10, "partition": "A"},
    // {_id: 1, "x": -3, "y": 8, "partition": "A"},
    // {_id: 5, "x": -6, "y": 6, "partition": "A"},
    // {_id: 2, "x": 10, "y": 4, "partition": "B"},
    // {_id: 4, "x": 2, "y": 3, "partition": "B"},
    // {_id: 3, "x": 3, "y": -2, "partition": "A"},
    // {_id: 6, "x": 0, "y": -8, "partition": "B"},
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {y: -1},
            output: {
                "relativeXValue":
                    {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
            }
        },
        docIdToOutputFieldValue: {
            1: 3 / 16,  // includes [-3, -6, 10, 2, 3, 0]
            2: 1,       // includes [10, 2, 3, 0]
            3: 1,       // includes [3, 0]
            4: 2 / 3,   // includes [2, 3, 0]
            5: 0,       // includes [-6, 10, 2, 3, 0]
            6: 0,       // includes [0]
            7: 5 / 8,   // includes all
        }
    });
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {documents: [-2, 2]}},
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
                        $minMaxScaler: {input: "$x"},
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
                        $minMaxScaler: {input: "$x", min: 10000, max: 20000},
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
        // Testing a more complex input expression.
        {
            setWindowFieldsArgs: {
                sortBy: {_id: 1},
                output: {
                    "relativeXValue": {
                        $minMaxScaler: {input: {$add: [1, "$x"]}},
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
                        $minMaxScaler: {input: {$const: 1}},
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
                    "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {documents: [-1, 2]}},
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

// Tests that range based bounds queries with removable windows produce the correct output
// field value. Removable queries are those which are bounded on the left
// (some values are removed from the window).
function testRangeBasedRemovableQueries() {
    validateTestCase({
        setWindowFieldsArgs: {
            sortBy: {_id: 1},
            output: {
                "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: ["current", 2]}},
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
                    "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: [-5, 5]}},
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
                    "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: [-3, 4]}},
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

testDocumentBasedNonRemovableQueries();
testRangeBasedNonRemovableQueries();
testDocumentBasedRemovableQueries();
testRangeBasedRemovableQueries();
