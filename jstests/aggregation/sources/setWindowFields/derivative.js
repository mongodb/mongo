/**
 * Test the behavior of $derivative.
 */
(function() {
"use strict";

const coll = db.setWindowFields_derivative;

// The default window is usually [unbounded, unbounded], but this would be surprising for
// $derivative, so instead it has no default (it requires an explicit window).
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
// Because the derivative is the same irrespective of sort order (as long as we reexpress the
// bounds) we can compare this result with the result of the previous aggregation.
const resultDesc =
    coll.aggregate([
            {
                $setWindowFields: {
                    sortBy: {time: -1},
                    output: {
                        dy: {$derivative: {input: "$y"}, window: {documents: [-1, +3]}},
                    }
                }
            },
            {$unset: "_id"},
            {$sort: {time: 1}},
        ])
        .toArray();
assert.docEq(result, resultDesc);

// Example with range-based bounds.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 10},
    {time: 10, y: 12},
    {time: 11, y: 15},
    {time: 12, y: 19},
    {time: 13, y: 24},
    {time: 20, y: 30},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {$derivative: {input: "$y"}, window: {range: [-10, 0]}},
                         }
                     }
                 },
                 {$unset: "_id"},
                 {$sort: {time: 1}},
             ])
             .toArray();
assert.docEq(result, [
    {time: 0, y: 10, dy: null},
    {time: 10, y: 12, dy: (12 - 10) / (10 - 0)},
    {time: 11, y: 15, dy: (15 - 12) / (11 - 10)},
    {time: 12, y: 19, dy: (19 - 12) / (12 - 10)},
    {time: 13, y: 24, dy: (24 - 12) / (13 - 10)},
    {time: 20, y: 30, dy: (30 - 12) / (20 - 10)},
]);

// 'unit' only supports 'week' and smaller.
coll.drop();
function derivativeStage(unit) {
    const stage = {
        $setWindowFields: {
            sortBy: {time: 1},
            output: {
                dy: {
                    $derivative: {
                        input: "$y",
                    },
                    window: {documents: [-1, 0]}
                },
            }
        }
    };
    if (unit) {
        stage.$setWindowFields.output.dy.$derivative.unit = unit;
    }
    return stage;
}
function explainUnit(unit) {
    return coll.runCommand(
        {explain: {aggregate: coll.getName(), cursor: {}, pipeline: [derivativeStage(unit)]}});
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

// When the time field is numeric, 'unit' is not allowed.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 100},
    {time: 1, y: 100},
    {time: 2, y: 100},
]));
assert.throwsWithCode(() => coll.aggregate(derivativeStage('millisecond')).toArray(), 5624900);
result = coll.aggregate([derivativeStage(), {$unset: '_id'}]).toArray();
assert.sameMembers(result, [
    {time: 0, y: 100, dy: null},
    {time: 1, y: 100, dy: 0},
    {time: 2, y: 100, dy: 0},
]);

// When the time field is a Date, 'unit' is required.
coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5},
]));
assert.throwsWithCode(() => coll.aggregate(derivativeStage()).toArray(), 5624901);
result = coll.aggregate([derivativeStage('millisecond'), {$unset: '_id'}]).toArray();
assert.sameMembers(result, [
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5, dy: null},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4, dy: -1},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6, dy: +2},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5, dy: -1},
]);

// The change per minute is 60*1000 larger than the change per millisecond.
result = coll.aggregate([derivativeStage('minute'), {$unset: "_id"}]).toArray();
assert.sameMembers(result, [
    // 'unit' applied to an ISODate expresses the output in terms of that unit.
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 5, dy: null},
    {time: ISODate("2020-01-01T00:00:00.001Z"), y: 4, dy: -1 * 60 * 1000},
    {time: ISODate("2020-01-01T00:00:00.002Z"), y: 6, dy: +2 * 60 * 1000},
    {time: ISODate("2020-01-01T00:00:00.003Z"), y: 5, dy: -1 * 60 * 1000},
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
                                 $derivative: {input: "$y", unit: 'millisecond'},
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

// When the sortBy field is a mixture of dates and numbers, it's an error:
// whether or not you specify unit, either the date or the number values
// will be an invalid type.
coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate("2020-01-01T00:00:00.000Z"), y: 0},
    {time: ISODate("2020-01-01T00:01:00.000Z"), y: 0},
    {time: 12, y: 0},
    {time: 13, y: 0},
]));
assert.throwsWithCode(() => coll.aggregate(derivativeStage()).toArray(), 5624901);
assert.throwsWithCode(() => coll.aggregate(derivativeStage('second')).toArray(), 5624900);

// Some examples of unbounded windows.
coll.drop();
assert.commandWorked(coll.insert([
    {time: 0, y: 0},
    {time: 1, y: 1},
    {time: 2, y: 4},
    {time: 3, y: 9},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y"},
                                 window: {
                                     documents: ['unbounded', 'unbounded'],
                                 }
                             }
                         }
                     }
                 },
                 {$unset: '_id'},
             ])
             .toArray();
assert.sameMembers(result, [
    {time: 0, y: 0, dy: 9 / 3},
    {time: 1, y: 1, dy: 9 / 3},
    {time: 2, y: 4, dy: 9 / 3},
    {time: 3, y: 9, dy: 9 / 3},
]);

coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate('2020-01-01T00:00:00Z'), y: 0},
    {time: ISODate('2020-01-01T00:00:01Z'), y: 1},
    {time: ISODate('2020-01-01T00:00:02Z'), y: 4},
    {time: ISODate('2020-01-01T00:00:03Z'), y: 9},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y", unit: 'second'},
                                 window: {
                                     documents: ['unbounded', 'unbounded'],
                                 }
                             }
                         }
                     }
                 },
                 {$unset: '_id'},
             ])
             .toArray();
assert.sameMembers(result, [
    {time: ISODate('2020-01-01T00:00:00Z'), y: 0, dy: 9 / 3},
    {time: ISODate('2020-01-01T00:00:01Z'), y: 1, dy: 9 / 3},
    {time: ISODate('2020-01-01T00:00:02Z'), y: 4, dy: 9 / 3},
    {time: ISODate('2020-01-01T00:00:03Z'), y: 9, dy: 9 / 3},
]);

// Example with time-based bounds.
coll.drop();
assert.commandWorked(coll.insert([
    {time: ISODate("2020-01-01T00:00:00"), y: 10},
    {time: ISODate("2020-01-01T00:00:10"), y: 12},
    {time: ISODate("2020-01-01T00:00:11"), y: 15},
    {time: ISODate("2020-01-01T00:00:12"), y: 19},
    {time: ISODate("2020-01-01T00:00:13"), y: 24},
    {time: ISODate("2020-01-01T00:00:20"), y: 30},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {time: 1},
                         output: {
                             dy: {
                                 $derivative: {input: "$y", unit: 'second'},
                                 window: {range: [-10, 0], unit: 'second'}
                             },
                         }
                     }
                 },
                 {$unset: "_id"},
                 {$sort: {time: 1}},
             ])
             .toArray();
assert.docEq(result, [
    {time: ISODate("2020-01-01T00:00:00.00Z"), y: 10, dy: null},
    {time: ISODate("2020-01-01T00:00:10.00Z"), y: 12, dy: (12 - 10) / (10 - 0)},
    {time: ISODate("2020-01-01T00:00:11.00Z"), y: 15, dy: (15 - 12) / (11 - 10)},
    {time: ISODate("2020-01-01T00:00:12.00Z"), y: 19, dy: (19 - 12) / (12 - 10)},
    {time: ISODate("2020-01-01T00:00:13.00Z"), y: 24, dy: (24 - 12) / (13 - 10)},
    {time: ISODate("2020-01-01T00:00:20.00Z"), y: 30, dy: (30 - 12) / (20 - 10)},
]);
})();
