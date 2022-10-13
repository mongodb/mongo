/**
 * Test the behavior of $integral.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db.setWindowFields_integral;

// Like most other window functions, the default window for $integral is [unbounded, unbounded].
coll.drop();
assert.commandWorked(coll.insert([
    {x: 0, y: 0},
    {x: 1, y: 42},
    {x: 3, y: 67},
    {x: 7, y: 99},
    {x: 10, y: 20},
]));
let result = coll.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [
        {
            $setWindowFields: {
                sortBy: {x: 1},
                output: {
                    integral: {$integral: {input: "$y"}},
                }
            }
        },
    ]
});
assert.commandWorked(result);

// $integral never compares values from separate partitions.
coll.drop();
assert.commandWorked(coll.insert([
    {partitionID: 1, x: 0, y: 1},
    {partitionID: 1, x: 1, y: 2},
    {partitionID: 1, x: 2, y: 1},
    {partitionID: 1, x: 3, y: 4},

    {partitionID: 2, x: 0, y: 100},
    {partitionID: 2, x: 2, y: 105},
    {partitionID: 2, x: 4, y: 107},
    {partitionID: 2, x: 6, y: -100},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         partitionBy: "$partitionID",
                         sortBy: {x: 1},
                         output: {
                             integral: {$integral: {input: "$y"}, window: {documents: [-1, 0]}},
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    {partitionID: 1, x: 0, y: 1, integral: 0},
    {partitionID: 1, x: 1, y: 2, integral: 1.5},  // (1 + 2) * (1 - 0) / 2 = 1.5
    {partitionID: 1, x: 2, y: 1, integral: 1.5},  // (1 + 2) * (2 - 1) / 2 = 1.5
    {partitionID: 1, x: 3, y: 4, integral: 2.5},  // (4 + 1) * (3 - 2) / 2 = 2.5

    {partitionID: 2, x: 0, y: 100, integral: 0},    //
    {partitionID: 2, x: 2, y: 105, integral: 205},  // (100 + 105) * 2 / 2 = 205
    {partitionID: 2, x: 4, y: 107, integral: 212},  // (105 + 107) * 2 / 2 = 212
    {partitionID: 2, x: 6, y: -100, integral: 7},   // (107 - 100) * 2 / 2 = 7
]);
// Because the integral from a to b is the same as the inverse of the integral from b to a, we can
// invert the input, sort order, and bounds so that the results are the same as the previous
// integral.
const resultDesc = coll.aggregate([
                           {
                               $setWindowFields: {
                                   partitionBy: "$partitionID",
                                   sortBy: {x: -1},
                                   output: {
                                       integral: {
                                           $integral: {input: {$subtract: [0, "$y"]}},
                                           window: {documents: [0, +1]}
                                       },
                                   }
                               }
                           },
                           {$unset: "_id"},
                       ])
                       .toArray();
assert.sameMembers(result, resultDesc);

// 'unit' only supports 'week' and smaller.
coll.drop();
function explainUnit(unit) {
    return coll.runCommand({
        explain: {
            aggregate: coll.getName(),
            cursor: {},
            pipeline: [{
                $setWindowFields: {
                    sortBy: {x: 1},
                    output: {
                        integral: {
                            $integral: {
                                input: "$y",
                                unit: unit,
                            },
                            window: {documents: [-1, 1]}
                        },
                    }
                }
            }]
        }
    });
}
assert.commandFailedWithCode(explainUnit('year'), 5490710);
assert.commandFailedWithCode(explainUnit('quarter'), 5490710);
assert.commandFailedWithCode(explainUnit('month'), 5490710);
assert.commandWorked(explainUnit('week'));
assert.commandWorked(explainUnit('day'));
assert.commandWorked(explainUnit('hour'));
assert.commandWorked(explainUnit('minute'));
assert.commandWorked(explainUnit('second'));
assert.commandWorked(explainUnit('millisecond'));

// Test if 'unit' is specified. Date type input is supported.
coll.drop();
assert.commandWorked(coll.insert([
    {x: ISODate("2020-01-01T00:00:00.000Z"), y: 0},
    {x: ISODate("2020-01-01T00:00:00.002Z"), y: 2},
    {x: ISODate("2020-01-01T00:00:00.004Z"), y: 4},
    {x: ISODate("2020-01-01T00:00:00.006Z"), y: 6},
]));

const pipelineWithUnit = [
    {
        $setWindowFields: {
            sortBy: {x: 1},
            output: {
                integral: {$integral: {input: "$y", unit: 'second'}, window: {documents: [-1, 1]}},
            }
        }
    },
    {$unset: "_id"},
];
result = coll.aggregate(pipelineWithUnit).toArray();
assert.sameMembers(result, [
    // We should scale the result by 'millisecond/second'.
    {x: ISODate("2020-01-01T00:00:00.000Z"), y: 0, integral: 0.002},
    {x: ISODate("2020-01-01T00:00:00.002Z"), y: 2, integral: 0.008},
    {x: ISODate("2020-01-01T00:00:00.004Z"), y: 4, integral: 0.016},
    {x: ISODate("2020-01-01T00:00:00.006Z"), y: 6, integral: 0.010},
]);

const pipelineWithNoUnit = [
    {
        $setWindowFields: {
            sortBy: {x: 1},
            output: {
                integral: {$integral: {input: "$y"}, window: {documents: [-1, 1]}},
            }
        }
    },
    {$unset: "_id"},
];
// 'unit' is only valid if the 'sortBy' values are ISODate objects.
// Dates are only valid if 'unit' is specified.
coll.drop();
assert.commandWorked(coll.insert([
    {x: 0, y: 100},
    {x: 1, y: 100},
    {x: ISODate("2020-01-01T00:00:00.000Z"), y: 5},
    {x: ISODate("2020-01-01T00:00:00.001Z"), y: 4},
]));
assert.commandFailedWithCode(db.runCommand({
    aggregate: "setWindowFields_integral",
    pipeline: pipelineWithUnit,
    cursor: {},
}),
                             5423901);

assert.commandFailedWithCode(db.runCommand({
    aggregate: "setWindowFields_integral",
    pipeline: pipelineWithNoUnit,
    cursor: {},
}),
                             5423902);

// Test various type of document-based window. Only test the stability not testing the actual
// result.
coll.drop();
assert.commandWorked(coll.insert([
    {x: 0, y: 0},
    {x: 1, y: 42},
    {x: 3, y: 67},
]));
documentBounds.forEach(function(bounds) {
    const res = assert.commandWorked(coll.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $setWindowFields: {
                    sortBy: {x: 1},
                    output: {
                        integral: {$integral: {input: "$y"}, window: {documents: bounds}},
                    }
                }
            },
        ]
    }));
    assert.eq(res.cursor.firstBatch.length, 3);
});

//
// Testing range-based $integral.
//
coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 0.0, integral: 0.002},
    {time: ISODate("2020-01-01T00:00:04.000Z"), y: 2.4, integral: 0.008},
    {time: ISODate("2020-01-01T00:00:10.000Z"), y: 5.6, integral: 0.016},
    {time: ISODate("2020-01-01T00:00:18.000Z"), y: 6.8, integral: 0.010},
]));

function runRangeBasedIntegral(bounds) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    sortBy: {time: 1},
                    output: {
                        integral: {
                            $integral: {input: "$y", unit: "second"},
                            window: {range: bounds, unit: "second"}
                        },
                    }
                }
            },
            {$unset: "_id"},
        ])
        .toArray();
}

// Empty window.
assert.sameMembers(runRangeBasedIntegral([-1, -1]), [
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 0.0, integral: null},
    {time: ISODate("2020-01-01T00:00:04.000Z"), y: 2.4, integral: null},
    {time: ISODate("2020-01-01T00:00:10.000Z"), y: 5.6, integral: null},
    {time: ISODate("2020-01-01T00:00:18.000Z"), y: 6.8, integral: null},
]);

// Window contains only one doc.
assert.sameMembers(runRangeBasedIntegral([-2, 0]), [
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 0.0, integral: 0},
    {time: ISODate("2020-01-01T00:00:04.000Z"), y: 2.4, integral: 0},
    {time: ISODate("2020-01-01T00:00:10.000Z"), y: 5.6, integral: 0},
    {time: ISODate("2020-01-01T00:00:18.000Z"), y: 6.8, integral: 0},
]);

// Window contains multiple docs.
assert.sameMembers(runRangeBasedIntegral([-6, 6]), [
    // doc[0] and doc[1] are in the window.
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 0.0, integral: 4.8},
    // doc[0], doc[1] and doc[2] are in the window.
    {time: ISODate("2020-01-01T00:00:04.000Z"), y: 2.4, integral: 28.8},
    // doc[1] and doc[2] are in the window.
    {time: ISODate("2020-01-01T00:00:10.000Z"), y: 5.6, integral: 24.0},
    // Empty window.
    {time: ISODate("2020-01-01T00:00:18.000Z"), y: 6.8, integral: 0.0},
]);
})();
