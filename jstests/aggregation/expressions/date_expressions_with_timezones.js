// Basic tests for using date expressions with time zone arguments.
(function() {
"use strict";

const coll = db.date_expressions_with_time_zones;
coll.drop();

assert.commandWorked(coll.insert([
    // Three sales on 2017-06-16 in UTC.
    {_id: 0, date: new ISODate("2017-06-16T00:00:00.000Z"), sales: 1},
    {_id: 1, date: new ISODate("2017-06-16T12:02:21.013Z"), sales: 2},
    // Six sales on 2017-06-17 in UTC.
    {_id: 2, date: new ISODate("2017-06-17T00:00:00.000Z"), sales: 2},
    {_id: 3, date: new ISODate("2017-06-17T12:02:21.013Z"), sales: 2},
    {_id: 4, date: new ISODate("2017-06-17T15:00:33.101Z"), sales: 2},
]));

// Compute how many sales happened on each day, in UTC.
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 3},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 6}
    ],
    coll.aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: "$date"},
                        month: {$month: "$date"},
                        day: {$dayOfMonth: "$date"}
                    },
                    totalSales: {$sum: "$sales"}
                }
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}}
        ])
        .toArray());

// Compute how many sales happened on each day, in New York. The sales made at midnight should
// move to the previous days.
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 15}, totalSales: 1},
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 4},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 4}
    ],
    coll.aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: {date: "$date", timezone: "America/New_York"}},
                        month: {$month: {date: "$date", timezone: "America/New_York"}},
                        day: {$dayOfMonth: {date: "$date", timezone: "America/New_York"}}
                    },
                    totalSales: {$sum: "$sales"}
                }
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}}
        ])
        .toArray());

// Compute how many sales happened on each day, in Sydney (+10 hours).
assert.eq(
    [
        {_id: {year: 2017, month: 6, day: 16}, totalSales: 3},
        {_id: {year: 2017, month: 6, day: 17}, totalSales: 4},
        {_id: {year: 2017, month: 6, day: 18}, totalSales: 2}
    ],
    coll.aggregate([
            {
                $group: {
                    _id: {
                        year: {$year: {date: "$date", timezone: "Australia/Sydney"}},
                        month: {$month: {date: "$date", timezone: "Australia/Sydney"}},
                        day: {$dayOfMonth: {date: "$date", timezone: "Australia/Sydney"}}
                    },
                    totalSales: {$sum: "$sales"}
                }
            },
            {$sort: {"_id.year": 1, "_id.month": 1, "_id.day": 1}}
        ])
        .toArray());

assert(coll.drop());
assert.commandWorked(coll.insert({}));

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
