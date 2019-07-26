load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and assertErrMsgContains.

(function() {
"use strict";

const coll = db.dateFromParts;

/* --------------------------------------------------------------------------------------- */
/* Basic Sanity Checks */
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713},
    {
        _id: 1,
        year: 2017,
        month: 6,
        day: 19,
        hour: 15,
        minute: 13,
        second: 25,
        millisecond: 713,
        timezone: "Europe/Amsterdam"
    },
    {
        _id: 2,
        year: 2017,
        month: 6,
        day: 19,
        hour: 15,
        minute: 13,
        second: 25,
        millisecond: 713,
        timezone: "Asia/Tokyo"
    },
    {
        _id: 3,
        date: {
            year: 2017,
            month: 6,
            day: 19,
            hour: 15,
            minute: 13,
            second: 25,
            millisecond: 713,
            timezone: "America/Chicago"
        }
    },
]));

assert.eq(
    [
        {_id: 1, date: ISODate("2016-12-31T23:00:00Z")},
        {_id: 2, date: ISODate("2016-12-31T15:00:00Z")},
    ],
    coll.aggregate([
            {
                $match: {'year': {$exists: true}, 'timezone': {$exists: true}},
            },
            {$project: {date: {'$dateFromParts': {year: "$year", "timezone": "$timezone"}}}}
        ])
        .toArray());

assert.eq(
    [
        {_id: 3, date: ISODate("2017-06-19T05:00:00Z")},
    ],
    coll.aggregate([
            {
                $match: {
                    'date.year': {$exists: true},
                },
            },
            {
                $project: {
                    date: {
                        '$dateFromParts': {
                            year: "$date.year",
                            month: '$date.month',
                            day: '$date.day',
                            timezone: '$date.timezone'
                        }
                    }
                }
            }
        ])
        .toArray());

let pipeline = {$project: {date: {'$dateFromParts': "$date"}}};
assertErrorCode(coll, pipeline, 40519);

pipeline = {
    $project: {date: {'$dateFromParts': {"timezone": "$timezone"}}}
};
assertErrorCode(coll, pipeline, 40516);

pipeline = {
    $project: {date: {'$dateFromParts': {year: false}}}
};
assertErrorCode(coll, pipeline, 40515);

pipeline = {
    $project: {date: {'$dateFromParts': {year: 2012, "timezone": "DoesNot/Exist"}}}
};
assertErrorCode(coll, pipeline, 40485);

pipeline = {
    $project: {date: {'$dateFromParts': {year: 2012, "timezone": 5}}}
};
assertErrorCode(coll, pipeline, 40517);

/* --------------------------------------------------------------------------------------- */

coll.drop();

assert.commandWorked(coll.insert([
    {
        _id: 0,
        year: 2017,
        month: 6,
        day: 23,
        hour: 14,
        minute: 27,
        second: 37,
        millisecond: 742,
        timezone: "Europe/Berlin"
    },
]));

let pipelines = [
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: NumberInt("2017"),
                    month: NumberInt("6"),
                    day: NumberInt("23"),
                    hour: NumberInt("14"),
                    minute: NumberInt("27"),
                    second: NumberInt("37"),
                    millisecond: NumberInt("742")
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: NumberLong("2017"),
                    month: NumberLong("6"),
                    day: NumberLong("23"),
                    hour: NumberLong("14"),
                    minute: NumberLong("27"),
                    second: NumberLong("37"),
                    millisecond: NumberLong("742")
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: NumberDecimal("2017"),
                    month: NumberDecimal("6"),
                    day: NumberDecimal("23"),
                    hour: NumberDecimal("14"),
                    minute: NumberDecimal("27"),
                    second: NumberDecimal("37"),
                    millisecond: NumberDecimal("742")
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "+02:00",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "-02",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 10,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "+02:00",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "+04:15",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 16,
                    minute: 42,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "$timezone",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: "$year",
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: "$month",
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: "$day",
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: "$hour",
                    minute: 27,
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: "$minute",
                    second: 37,
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: "$second",
                    millisecond: 742
                }
            }
        }
    }],
    [{
        '$project': {
            date: {
                '$dateFromParts': {
                    timezone: "Europe/Berlin",
                    year: 2017,
                    month: 6,
                    day: 23,
                    hour: 14,
                    minute: 27,
                    second: 37,
                    millisecond: "$millisecond"
                }
            }
        }
    }],
];

pipelines.forEach(function(pipeline) {
    assert.eq([{_id: 0, date: ISODate("2017-06-23T12:27:37.742Z")}],
              coll.aggregate(pipeline).toArray(),
              tojson(pipeline));
});

/* --------------------------------------------------------------------------------------- */
/* Testing whether it throws the right assert for missing values */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0},
]));

pipelines = [
    [{'$project': {date: {'$dateFromParts': {year: "$year"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, month: "$month"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, day: "$day"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, hour: "$hour"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, minute: "$minute"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, second: "$second"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, millisecond: "$millisecond"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: "$isoWeekYear"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoWeek: "$isoWeek"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: "$isoDayOfWeek"}}}}],
];

pipelines.forEach(function(pipeline) {
    assert.eq([{_id: 0, date: null}], coll.aggregate(pipeline).toArray(), tojson(pipeline));
});

pipeline = [{'$project': {date: {'$dateFromParts': {year: 2017, timezone: "$timezone"}}}}];
assert.eq([{_id: 0, date: null}], coll.aggregate(pipeline).toArray());

/* --------------------------------------------------------------------------------------- */
/* Testing whether it throws the right assert for uncoersable values */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, falseValue: false},
]));

pipelines = [
    [{'$project': {date: {'$dateFromParts': {year: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, month: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, day: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, hour: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, minute: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, second: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 2017, millisecond: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoWeek: "$falseValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: "$falseValue"}}}}],
];

pipelines.forEach(function(pipeline) {
    assertErrorCode(coll, pipeline, 40515, tojson(pipeline));
});

pipeline = [{'$project': {date: {'$dateFromParts': {year: 2017, timezone: "$falseValue"}}}}];
assertErrorCode(coll, pipeline, 40517);

/* --------------------------------------------------------------------------------------- */
/* Testing whether it throws the right assert for uncoersable values */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, outOfRangeValue: 10002},
]));

pipelines = [
    [{'$project': {date: {'$dateFromParts': {year: "$outOfRangeValue"}}}}],
    [{'$project': {date: {'$dateFromParts': {year: -1}}}}],
    [{'$project': {date: {'$dateFromParts': {year: 10000}}}}],
];

pipelines.forEach(function(pipeline) {
    assertErrorCode(coll, pipeline, 40523, tojson(pipeline));
});

/* --------------------------------------------------------------------------------------- */
/* Testing "out of range" under and overflows */

coll.drop();

assert.commandWorked(coll.insert([{
    _id: 0,
    minusOne: -1,
    zero: 0,
    thirteen: 13,
    twentyFive: 25,
    sixtyOne: 61,
    thousandAndOne: 1001,
    tenThousandMinusOne: 9999,
    tenThousandAndOne: 10001,
    seventyMillionAndSomething: 71841012,
    secondsSinceEpoch: 1502095918,
    millisSinceEpoch: NumberLong("1502095918551"),
}]));

tests = [
    {expected: "0000-01-01T00:00:00.000Z", parts: {year: "$zero"}},
    {expected: "9999-01-01T00:00:00.000Z", parts: {year: "$tenThousandMinusOne"}},
    {expected: "2016-11-01T00:00:00.000Z", parts: {year: 2017, month: "$minusOne"}},
    {expected: "2016-12-01T00:00:00.000Z", parts: {year: 2017, month: "$zero"}},
    {expected: "2018-01-01T00:00:00.000Z", parts: {year: 2017, month: "$thirteen"}},
    {expected: "2016-12-30T00:00:00.000Z", parts: {year: 2017, day: "$minusOne"}},
    {expected: "2016-12-31T00:00:00.000Z", parts: {year: 2017, day: "$zero"}},
    {expected: "2017-03-02T00:00:00.000Z", parts: {year: 2017, day: "$sixtyOne"}},
    {expected: "2016-12-31T23:00:00.000Z", parts: {year: 2017, hour: "$minusOne"}},
    {expected: "2017-01-02T01:00:00.000Z", parts: {year: 2017, hour: "$twentyFive"}},
    {expected: "2016-12-31T23:59:00.000Z", parts: {year: 2017, minute: "$minusOne"}},
    {expected: "2017-01-01T00:00:00.000Z", parts: {year: 2017, minute: "$zero"}},
    {expected: "2017-01-01T01:01:00.000Z", parts: {year: 2017, minute: "$sixtyOne"}},
    {expected: "2016-12-31T23:59:59.000Z", parts: {year: 2017, second: "$minusOne"}},
    {expected: "2017-01-01T00:01:01.000Z", parts: {year: 2017, second: "$sixtyOne"}},
    {
        expected: "2019-04-12T11:50:12.000Z",
        parts: {year: 2017, second: "$seventyMillionAndSomething"}
    },
    {
        expected: "1972-04-11T11:50:12.000Z",
        parts: {year: 1970, second: "$seventyMillionAndSomething"}
    },
    {expected: "2017-08-07T08:51:58.000Z", parts: {year: 1970, second: "$secondsSinceEpoch"}},
    {expected: "2016-12-31T23:59:59.999Z", parts: {year: 2017, millisecond: "$minusOne"}},
    {expected: "2017-01-01T00:00:01.001Z", parts: {year: 2017, millisecond: "$thousandAndOne"}},
    {
        expected: "2017-01-01T19:57:21.012Z",
        parts: {year: 2017, millisecond: "$seventyMillionAndSomething"}
    },
    {expected: "2017-01-18T09:14:55.918Z", parts: {year: 2017, millisecond: "$secondsSinceEpoch"}},
    {
        expected: "1970-01-01T19:57:21.012Z",
        parts: {year: 1970, millisecond: "$seventyMillionAndSomething"}
    },
    {expected: "2017-08-07T08:51:58.551Z", parts: {year: 1970, millisecond: "$millisSinceEpoch"}},
];

tests.forEach(function(test) {
    assert.eq(
        [
            {_id: 0, date: ISODate(test.expected)},
        ],
        coll.aggregate([{$project: {date: {"$dateFromParts": test.parts}}}]).toArray(),
        tojson(test));
});

/* --------------------------------------------------------------------------------------- */
/*
 * Testing double and Decimal128 millisecond values that aren't representable as a 64-bit
 * integer or overflow when converting to a 64-bit microsecond value.
 */
coll.drop();

assert.commandWorked(coll.insert([{
    _id: 0,
    veryBigDoubleA: 18014398509481984.0,
    veryBigDecimal128A: NumberDecimal("9223372036854775807"),  // 2^63-1
    veryBigDoubleB: 18014398509481984000.0,
    veryBigDecimal128B: NumberDecimal("9223372036854775807000"),  // (2^63-1) * 1000
}]));

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, millisecond: "$veryBigDoubleA"}}}}];
assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    ErrorCodes.DurationOverflow,
    "Overflow casting from a lower-precision duration to a higher-precision duration");

pipeline =
    [{$project: {date: {"$dateFromParts": {year: 1970, millisecond: "$veryBigDecimal128A"}}}}];
assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    ErrorCodes.DurationOverflow,
    "Overflow casting from a lower-precision duration to a higher-precision duration");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, millisecond: "$veryBigDoubleB"}}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, 40515, "'millisecond' must evaluate to an integer");

pipeline =
    [{$project: {date: {"$dateFromParts": {year: 1970, millisecond: "$veryBigDecimal128B"}}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, 40515, "'millisecond' must evaluate to an integer");

/* --------------------------------------------------------------------------------------- */
/* Testing that year values are only allowed in the range [0, 9999] and that month, day, hour,
 * and minute values are only allowed in the range [-32,768, 32,767]. */
coll.drop();

assert.commandWorked(coll.insert(
    [{_id: 0, bigYear: 10000, smallYear: -1, prettyBigInt: 32768, prettyBigNegativeInt: -32769}]));

pipeline = [{$project: {date: {"$dateFromParts": {year: "$bigYear"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 40523, "'year' must evaluate to an integer in the range 0 to 9999");

pipeline = [{$project: {date: {"$dateFromParts": {year: "$smallYear"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 40523, "'year' must evaluate to an integer in the range 0 to 9999");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, month: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'month' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, month: "$prettyBigNegativeInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'month' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, month: 1, day: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'day' must evaluate to a value in the range [-32768, 32767]");

pipeline =
    [{$project: {date: {"$dateFromParts": {year: 1970, month: 1, day: "$prettyBigNegativeInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'day' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, hour: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'hour' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, hour: "$prettyBigNegativeInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'hour' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {year: 1970, hour: 0, minute: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'minute' must evaluate to a value in the range [-32768, 32767]");

pipeline = [
    {$project: {date: {"$dateFromParts": {year: 1970, hour: 0, minute: "$prettyBigNegativeInt"}}}}
];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'minute' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{$project: {date: {"$dateFromParts": {isoWeekYear: "$bigYear"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31095, "'isoWeekYear' must evaluate to an integer in the range 0 to 9999");

pipeline = [{$project: {date: {"$dateFromParts": {isoWeekYear: "$smallYear"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31095, "'isoWeekYear' must evaluate to an integer in the range 0 to 9999");

pipeline = [{$project: {date: {"$dateFromParts": {isoWeekYear: 1970, isoWeek: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'isoWeek' must evaluate to a value in the range [-32768, 32767]");

pipeline =
    [{$project: {date: {"$dateFromParts": {isoWeekYear: 1970, isoWeek: "$prettyBigNegativeInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'isoWeek' must evaluate to a value in the range [-32768, 32767]");

pipeline =
    [{$project: {date: {"$dateFromParts": {isoWeekYear: 1970, isoDayOfWeek: "$prettyBigInt"}}}}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'isoDayOfWeek' must evaluate to a value in the range [-32768, 32767]");

pipeline = [{
    $project: {date: {"$dateFromParts": {isoWeekYear: 1970, isoDayOfWeek: "$prettyBigNegativeInt"}}}
}];
assertErrCodeAndErrMsgContains(
    coll, pipeline, 31034, "'isoDayOfWeek' must evaluate to a value in the range [-32768, 32767]");

/* --------------------------------------------------------------------------------------- */
/* Testing wrong arguments */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0},
]));

pipelines = [
    {code: 40519, pipeline: {'$project': {date: {'$dateFromParts': true}}}},
    {code: 40519, pipeline: {'$project': {date: {'$dateFromParts': []}}}},

    {code: 40518, pipeline: {'$project': {date: {'$dateFromParts': {unknown: true}}}}},

    {code: 40516, pipeline: {'$project': {date: {'$dateFromParts': {}}}}},

    {
        code: 40489,
        pipeline: {'$project': {date: {'$dateFromParts': {year: 2017, isoWeekYear: 2017}}}}
    },
    {code: 40489, pipeline: {'$project': {date: {'$dateFromParts': {year: 2017, isoWeek: 3}}}}},
    {
        code: 40489,
        pipeline: {'$project': {date: {'$dateFromParts': {year: 2017, isoDayOfWeek: 5}}}}
    },
    {
        code: 40489,
        pipeline: {'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, year: 2017}}}}
    },

    {
        code: 40525,
        pipeline: {'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, month: 12}}}}
    },
    {code: 40525, pipeline: {'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, day: 17}}}}},
];

pipelines.forEach(function(item) {
    assertErrorCode(coll, item.pipeline, item.code, tojson(pipeline));
});

/* --------------------------------------------------------------------------------------- */
/* Testing wrong value (types) */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, floatField: 2017.5, decimalField: NumberDecimal("2017.5")},
]));

pipelines = [
    {code: 40515, pipeline: {'$project': {date: {'$dateFromParts': {year: "2017"}}}}},
    {code: 40515, pipeline: {'$project': {date: {'$dateFromParts': {year: 2017.3}}}}},
    {
        code: 40515,
        pipeline: {'$project': {date: {'$dateFromParts': {year: NumberDecimal("2017.3")}}}}
    },
    {code: 40515, pipeline: {'$project': {date: {'$dateFromParts': {year: "$floatField"}}}}},
    {code: 40515, pipeline: {'$project': {date: {'$dateFromParts': {year: "$decimalField"}}}}},
];

pipelines.forEach(function(item) {
    assertErrorCode(coll, item.pipeline, item.code, tojson(pipeline));
});

/* --------------------------------------------------------------------------------------- */

coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, year: NumberDecimal("2017"), month: 6.0, day: NumberInt(19), hour: NumberLong(15)},
    {
        _id: 1,
        year: NumberDecimal("2017"),
        minute: 6.0,
        second: NumberInt(19),
        millisecond: NumberLong(15)
    },
    {_id: 2, isoWeekYear: NumberDecimal("2017"), isoWeek: 6.0, isoDayOfWeek: NumberInt(4)},
]));

assert.eq(
    [
        {_id: 0, date: ISODate("2017-06-19T15:00:00Z")},
    ],
    coll.aggregate([
            {
                $match: {_id: 0},
            },
            {
                $project: {
                    date: {
                        '$dateFromParts':
                            {year: "$year", month: "$month", day: "$day", hour: "$hour"}
                    }
                }
            }
        ])
        .toArray());

assert.eq(
    [
        {_id: 1, date: ISODate("2017-01-01T00:06:19.015Z")},
    ],
    coll.aggregate([
            {
                $match: {_id: 1},
            },
            {
                $project: {
                    date: {
                        '$dateFromParts': {
                            year: "$year",
                            minute: "$minute",
                            second: "$second",
                            millisecond: "$millisecond"
                        }
                    }
                }
            }
        ])
        .toArray());

assert.eq(
    [
        {_id: 2, date: ISODate("2017-02-09T00:00:00Z")},
    ],
    coll.aggregate([
            {
                $match: {_id: 2},
            },
            {
                $project: {
                    date: {
                        '$dateFromParts': {
                            isoWeekYear: "$isoWeekYear",
                            isoWeek: "$isoWeek",
                            isoDayOfWeek: "$isoDayOfWeek"
                        }
                    }
                }
            }
        ])
        .toArray());

/* --------------------------------------------------------------------------------------- */

coll.drop();

assert.commandWorked(coll.insert([
    {
        _id: 0,
        year: NumberDecimal("2017"),
        month: 6.0,
        day: NumberInt(19),
        hour: NumberLong(15),
        minute: NumberDecimal(1),
        second: 51,
        millisecond: 551
    },
]));

var tests = [
    {expected: ISODate("2017-06-19T19:01:51.551Z"), tz: "-04:00"},
    {expected: ISODate("2017-06-19T12:01:51.551Z"), tz: "+03"},
    {expected: ISODate("2017-06-19T18:21:51.551Z"), tz: "-0320"},
    {expected: ISODate("2017-06-19T19:01:51.551Z"), tz: "America/New_York"},
    {expected: ISODate("2017-06-19T13:01:51.551Z"), tz: "Europe/Amsterdam"},
];

tests.forEach(function(test) {
    assert.eq(
        [
            {_id: 0, date: test.expected},
        ],
        coll.aggregate([{
                $project: {
                    date: {
                        "$dateFromParts": {
                            year: "$year",
                            month: "$month",
                            day: "$day",
                            hour: "$hour",
                            minute: "$minute",
                            second: "$second",
                            millisecond: "$millisecond",
                            timezone: test.tz
                        }
                    }
                }
            }])
            .toArray(),
        tojson(test));
});

/* --------------------------------------------------------------------------------------- */

coll.drop();

assert.commandWorked(coll.insert([
    {
        _id: 0,
        isoWeekYear: NumberDecimal("2017"),
        isoWeek: 25.0,
        isoDayOfWeek: NumberInt(1),
        hour: NumberLong(15),
        minute: NumberDecimal(1),
        second: 51,
        millisecond: 551
    },
]));

var tests = [
    {expected: ISODate("2017-06-19T19:01:51.551Z"), tz: "-04:00"},
    {expected: ISODate("2017-06-19T12:01:51.551Z"), tz: "+03"},
    {expected: ISODate("2017-06-19T18:21:51.551Z"), tz: "-0320"},
    {expected: ISODate("2017-06-19T19:01:51.551Z"), tz: "America/New_York"},
    {expected: ISODate("2017-06-19T13:01:51.551Z"), tz: "Europe/Amsterdam"},
];

tests.forEach(function(test) {
    assert.eq(
        [
            {_id: 0, date: test.expected},
        ],
        coll.aggregate([{
                $project: {
                    date: {
                        "$dateFromParts": {
                            isoWeekYear: "$isoWeekYear",
                            isoWeek: "$isoWeek",
                            isoDayOfWeek: "$isoDayOfWeek",
                            hour: "$hour",
                            minute: "$minute",
                            second: "$second",
                            millisecond: "$millisecond",
                            timezone: test.tz
                        }
                    }
                }
            }])
            .toArray(),
        tojson(test));
});

/* --------------------------------------------------------------------------------------- */
})();
