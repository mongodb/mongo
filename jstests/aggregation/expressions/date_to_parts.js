load("jstests/aggregation/extras/utils.js");  // For assertErrorCode
load("jstests/libs/sbe_assert_error_override.js");

(function() {
"use strict";

const coll = db.dateToParts;
coll.drop();

/* --------------------------------------------------------------------------------------- */
assert.commandWorked(coll.insert([
    {_id: 0, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "UTC"},
    {_id: 1, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "Europe/London"},
    {_id: 2, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "America/New_York", iso: true},
    {_id: 3, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "America/New_York", iso: false},
]));

assert.eq(
    [
        {
            _id: 0,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 1,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 2,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 3,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}, {$sort: {_id: 1}}])
        .toArray());

assert.eq(
    [
        {
            _id: 0,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 1,
            date:
                {year: 2017, month: 6, day: 19, hour: 16, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 2,
            date:
                {year: 2017, month: 6, day: 19, hour: 11, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 3,
            date:
                {year: 2017, month: 6, day: 19, hour: 11, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([
            {$project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz"}}}},
            {$sort: {_id: 1}}
        ])
        .toArray());

assert.eq(
    [
        {
            _id: 0,
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 1,
            date:
                {year: 2017, month: 6, day: 19, hour: 16, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 2,
            date:
                {year: 2017, month: 6, day: 19, hour: 11, minute: 13, second: 25, millisecond: 713}
        },
        {
            _id: 3,
            date:
                {year: 2017, month: 6, day: 19, hour: 11, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([
            {
                $project:
                    {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": false}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray());

assert.eq(
    [
        {
            _id: 0,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
        {
            _id: 1,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
        {
            _id: 2,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
        {
            _id: 3,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
    ],
    coll.aggregate([
            {
                $project:
                    {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": true}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray());

assert.eq(
    [
        {
            _id: 2,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
        {
            _id: 3,
            date:
                {year: 2017, month: 6, day: 19, hour: 11, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([
            {$match: {iso: {$exists: true}}},
            {
                $project:
                    {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": "$iso"}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray());

/* --------------------------------------------------------------------------------------- */
/* Tests with timestamp */
assert(coll.drop());

assert.commandWorked(coll.insert([
    {
        _id: ObjectId("58c7cba47bbadf523cf2c313"),
        date: new ISODate("2017-06-19T15:13:25.713Z"),
        tz: "Europe/London"
    },
]));

assert.eq(
    [
        {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date:
                {year: 2017, month: 6, day: 19, hour: 15, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}]).toArray());

assert.eq(
    [
        {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date:
                {year: 2017, month: 6, day: 19, hour: 16, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz"}}}}])
        .toArray());

assert.eq(
    [
        {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date:
                {year: 2017, month: 6, day: 19, hour: 16, minute: 13, second: 25, millisecond: 713}
        },
    ],
    coll.aggregate([{
            $project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": false}}}
        }])
        .toArray());

assert.eq(
    [
        {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
        },
    ],
    coll.aggregate([{
            $project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": true}}}
        }])
        .toArray());

assert.eq(
    [
        {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {year: 2017, month: 3, day: 14, hour: 10, minute: 53, second: 24, millisecond: 0}
        },
    ],
    coll.aggregate([{
            $project: {date: {'$dateToParts': {date: "$_id", "timezone": "$tz", "iso8601": false}}}
        }])
        .toArray());

/* --------------------------------------------------------------------------------------- */
assert(coll.drop());

assert.commandWorked(coll.insert([
    {_id: 0, date: ISODate("2017-06-27T12:00:20Z")},
]));

assert.eq(
    [
        {_id: 0, date: null},
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", timezone: "$tz"}}}}])
        .toArray());

/* --------------------------------------------------------------------------------------- */
assert(coll.drop());

assert.commandWorked(coll.insert([
    {_id: 0, date: ISODate("2017-06-27T12:00:20Z")},
]));

assert.eq(
    [
        {_id: 0, date: null},
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", iso8601: "$iso8601"}}}}])
        .toArray());

/* --------------------------------------------------------------------------------------- */
assert(coll.drop());

assert.commandWorked(coll.insert([
    {_id: 0, tz: "Europe/London"},
]));

assert.eq(
    [
        {_id: 0, date: null},
    ],
    coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}]).toArray());

/* --------------------------------------------------------------------------------------- */

let pipeline = {$project: {date: {'$dateToParts': {"timezone": "$tz"}}}};
assertErrorCode(coll, pipeline, 40522);

pipeline = {
    $project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": 5}}}
};
assertErrorCode(coll, pipeline, 40521);

pipeline = {
    $project: {date: {'$dateToParts': {date: 42}}}
};
assertErrorCode(coll, pipeline, 16006);

pipeline = {
    $project: {date: {'$dateToParts': {date: "$date", "timezone": 5}}}
};
assertErrorCode(coll, pipeline, 40517);

pipeline = {
    $project: {date: {'$dateToParts': {date: "$date", "timezone": "DoesNot/Exist"}}}
};
assertErrorCode(coll, pipeline, 40485);

// Given a date, executes an aggregation command which requires the server to convert this date to
// its parts in a timezone-aware fashion. Returns the resulting document containing the date parts.
function runDateToPartsExpression(inputDate, timezone) {
    const results =
        coll.aggregate(
                [{$project: {_id: 0, out: {$dateToParts: {date: inputDate, timezone: timezone}}}}])
            .toArray();
    assert.eq(results.length, 1, results);
    return results[0].out;
}

// Test that the time zone info is up to date as of the 2019c IANA tz db release. In 2019, Brazil
// abolished its daylight savings time. In recent years prior to 2019, Brazil would change from
// UTC-3 to UTC-2, typically starting in October or November and ending in February. Here we test
// that dates in December and January use UTC-2 in 2017 and 2018, but use UTC-3 in 2019 and 2020 for
// the Sao Paulo timezone.
assert.eq({year: 2017, month: 12, day: 15, hour: 8, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2017-12-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2018, month: 1, day: 15, hour: 8, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2018-01-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2018, month: 12, day: 15, hour: 8, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2018-12-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2019, month: 1, day: 15, hour: 8, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2019-01-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2019, month: 12, day: 15, hour: 7, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2019-12-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2020, month: 1, day: 15, hour: 7, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2020-01-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2020, month: 12, day: 15, hour: 7, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2020-12-15T10:00:00.000Z"), "America/Sao_Paulo"));
assert.eq({year: 2021, month: 1, day: 15, hour: 7, minute: 0, second: 0, millisecond: 0},
          runDateToPartsExpression(ISODate("2021-01-15T10:00:00.000Z"), "America/Sao_Paulo"));
})();
