/**
 * Test the behavior of $derivative.
 */
(function() {
"use strict";

const getParam = db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1});
jsTestLog(getParam);
const featureEnabled = assert.commandWorked(getParam).featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db.setWindowFields_derivative;

// Like most other window functions, the default window for $derivative is [unbounded, unbounded].
// This may be a surprising default.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 0},
    {time: 1, y: 42},
    {time: 3, y: 67},
    {time: 7, y: 99},
    {time: 10, y: 20},
]));
let result = coll.runCommand({
    explain: {
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $setWindowFields: {
                    sortBy: {time: 1},
                    output: {
                        dy: {$derivative: {input: "$y"}},
                    }
                }
            },
        ]
    }
});
assert.commandFailedWithCode(
    result, ErrorCodes.FailedToParse, "$derivative requires explicit window bounds");

// $derivative never compares values from separate partitions.
coll.drop();
assert.commandWorked(coll.insert([
    {sensor: "A", time: 0, y: 1},
    {sensor: "A", time: 1, y: 2},
    {sensor: "A", time: 2, y: 1},
    {sensor: "A", time: 3, y: 4},

    {sensor: "B", time: 0, y: 100},
    {sensor: "B", time: 1, y: 105},
    {sensor: "B", time: 2, y: 107},
    {sensor: "B", time: 3, y: 104},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         partitionBy: "$sensor",
                         sortBy: {time: 1},
                         output: {
                             dy: {$derivative: {input: "$y"}, window: {documents: [-1, 0]}},
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    {sensor: "A", time: 0, y: 1, dy: null},
    {sensor: "A", time: 1, y: 2, dy: +1},
    {sensor: "A", time: 2, y: 1, dy: -1},
    {sensor: "A", time: 3, y: 4, dy: +3},

    {sensor: "B", time: 0, y: 100, dy: null},
    {sensor: "B", time: 1, y: 105, dy: +5},
    {sensor: "B", time: 2, y: 107, dy: +2},
    {sensor: "B", time: 3, y: 104, dy: -3},
]);

// When either endpoint lies outside the partition, we use the first/last document in the partition
// instead.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 100},
    {time: 1, y: 105},
    {time: 2, y: 108},
    {time: 3, y: 108},
    {time: 4, y: 115},
    {time: 5, y: 115},
    {time: 6, y: 118},
    {time: 7, y: 118},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {$derivative: {input: "$y"}, window: {documents: [-3, +1]}},
                         }
                     }
                 },
                 {$unset: "_id"},
                 {$sort: {time: 1}},
             ])
             .toArray();
assert.docEq(result, [
    // The first document looks behind 3, but can't go any further back than time: 0.
    // It also looks ahead 1. So the points it compares are time: 0 and time: 1.
    {time: 0, y: 100, dy: +5},
    // The second document gets time: 0 and time: 2.
    {time: 1, y: 105, dy: +8 / 2},
    // The third gets time: 0 and time: 3.
    {time: 2, y: 108, dy: +8 / 3},
    // This is the first document whose left endpoint lies within the partition.
    // So this one, and the next few, all have fully-populated windows.
    {time: 3, y: 108, dy: +15 / 4},
    {time: 4, y: 115, dy: +10 / 4},
    {time: 5, y: 115, dy: +10 / 4},
    {time: 6, y: 118, dy: +10 / 4},
    // For the last document, there is no document at offset +1, so it sees
    // time: 4 and time: 7.
    {time: 7, y: 118, dy: +3 / 3},
]);

// 'outputUnit' only supports 'week' and smaller.
coll.drop();
function explainUnit(outputUnit) {
    return coll.runCommand({
        explain: {
            aggregate: coll.getName(),
            cursor: {},
            pipeline: [{
                $setWindowFields: {
                    sortBy: {time: 1},
                    output: {
                        dy: {
                            $derivative: {
                                input: "$y",
                                outputUnit: outputUnit,
                            },
                            window: {documents: [-1, 0]}
                        },
                    }
                }
            }]
        }
    });
}
assert.commandFailedWithCode(explainUnit('year'), 5490704);
assert.commandFailedWithCode(explainUnit('quarter'), 5490704);
assert.commandFailedWithCode(explainUnit('month'), 5490704);
assert.commandWorked(explainUnit('week'));
assert.commandWorked(explainUnit('day'));
assert.commandWorked(explainUnit('hour'));
assert.commandWorked(explainUnit('minute'));
assert.commandWorked(explainUnit('second'));
assert.commandWorked(explainUnit('millisecond'));

// 'outputUnit' is only valid if the time values are ISODate objects.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 100},
    {time: 1, y: 100},
    {time: 2, y: 100},
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y", outputUnit: 'millisecond'},
                                 window: {documents: [-1, 0]}
                             },
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    // 'outputUnit' applied to an ISODate expresses the output in terms of that unit.
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5, dy: null},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4, dy: -1},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6, dy: +2},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5, dy: -1},
    // 'outputUnit' applied to a non-ISODate is not allowed... we render it as null.
    {time: 0, y: 100, dy: null},
    {time: 1, y: 100, dy: null},
    {time: 2, y: 100, dy: null},
]);
// The change per minute is 60*1000 larger than the change per millisecond.
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y", outputUnit: 'minute'},
                                 window: {documents: [-1, 0]}
                             },
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    // 'outputUnit' applied to an ISODate expresses the output in terms of that unit.
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5, dy: null},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4, dy: -1 * 60 * 1000},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6, dy: +2 * 60 * 1000},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5, dy: -1 * 60 * 1000},
    // It's still null for the non-ISODates.
    {time: 0, y: 100, dy: null},
    {time: 1, y: 100, dy: null},
    {time: 2, y: 100, dy: null},
]);

// Going the other direction: if the events are spaced far apart, expressing the answer in
// change-per-millisecond makes the result small.
coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5},
    {time: ISODate("2020-01-01T00:01:00.000Z"), y: 4},
    {time: ISODate("2020-01-01T00:02:00.000Z"), y: 6},
    {time: ISODate("2020-01-01T00:03:00.000Z"), y: 5},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y", outputUnit: 'millisecond'},
                                 window: {documents: [-1, 0]}
                             },
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5, dy: null},
    {time: ISODate("2020-01-01T00:01:00.000Z"), y: 4, dy: -1 / (60 * 1000)},
    {time: ISODate("2020-01-01T00:02:00.000Z"), y: 6, dy: +2 / (60 * 1000)},
    {time: ISODate("2020-01-01T00:03:00.000Z"), y: 5, dy: -1 / (60 * 1000)},
]);
})();
