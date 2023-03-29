// Basic tests for using date expressions with time zone arguments.
(function() {
"use strict";

load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

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

function runYearExpression(timezoneConst) {
    let t = timezoneConst ? timezoneConst : "$timezone";
    let pipeline = [{$project: {year: {$year: {date: "$date", timezone: t}}}}];
    return coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}});
}

// Test $year
assert(coll.drop());
assert.commandWorked(
    coll.insert({date: ISODate("2017-06-16T00:00:00.000Z"), timezone: "America/Sao_Paulo"}));
assert.eq(2017, runYearExpression().cursor.firstBatch[0].year);
assert.eq(2017, runYearExpression("America/Sao_Paulo").cursor.firstBatch[0].year);

// Test $year with offset based timezone
assert(coll.drop());
assert.commandWorked(coll.insert({date: ISODate("2017-01-01T00:00:00.000Z"), timezone: "-01:00"}));
assert.eq(2016, runYearExpression().cursor.firstBatch[0].year);
assert.eq(2016, runYearExpression("-01:00").cursor.firstBatch[0].year);

// Test $year when document has no $timezone field
assert(coll.drop());
assert.commandWorked(coll.insert({date: ISODate("2017-06-16T00:00:00.000Z")}));
assert.eq(null, runYearExpression().cursor.firstBatch[0].year);
assert.eq(2017, coll.aggregate([{$project: {year: {$year: {date: "$date"}}}}]).toArray()[0].year);

// Test $year when document has no date field
assert(coll.drop());
assert.commandWorked(coll.insert({timezone: "America/Sao_Paulo"}));
assert.eq(null, runYearExpression().cursor.firstBatch[0].year);

// test with invalid timezone identifier
assert(coll.drop());
assert.commandWorked(coll.insert({date: ISODate("2017-06-16T00:00:00.000Z"), timezone: "USA"}));
assert.commandFailedWithCode(runYearExpression(), 40485);
assert.commandFailedWithCode(runYearExpression("USA"), 40485);

// test with invalid timezone type
assert(coll.drop());
assert.commandWorked(coll.insert({date: ISODate("2017-06-16T00:00:00.000Z"), timezone: 123}));
assert.commandFailedWithCode(runYearExpression(), 40533);
assert.commandFailedWithCode(runYearExpression(1111), 40533);

// test with invalid date type
assert(coll.drop());
assert.commandWorked(
    coll.insert({date: "2017-06-16T00:00:00.000Z", timezone: "America/Sao_Paulo"}));
assert.commandFailedWithCode(runYearExpression(), 16006);
})();
