// Tests for the $dayOfYear, $dayOfMonth, and $dayOfWeek expressions.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode
load("jstests/libs/sbe_assert_error_override.js");

const coll = db.dayOfExpressions;

//
// Basic tests.
//
coll.drop();
assert.commandWorked(coll.insert([
    {date: ISODate('1960-01-02 03:04:05Z')},
    {date: ISODate('1970-01-01 00:00:00.000Z')},
    {date: ISODate('1980-05-20 12:53:64.834Z')},
    {date: ISODate('1999-12-31 00:00:00.000Z')},
]));

let res = coll.aggregate({
                  $project: {
                      _id: 0,
                      dayOfYear: {$dayOfYear: "$date"},
                      dayOfMonth: {$dayOfMonth: "$date"},
                      dayOfWeek: {$dayOfWeek: "$date"},
                  }
              })
              .toArray();
assert(arrayEq(res, [
    {dayOfYear: 2, dayOfMonth: 2, dayOfWeek: 7},
    {dayOfYear: 1, dayOfMonth: 1, dayOfWeek: 5},
    {dayOfYear: 141, dayOfMonth: 20, dayOfWeek: 3},
    {dayOfYear: 365, dayOfMonth: 31, dayOfWeek: 6}
]));

//
// Test date and timestamp dayOfYear equality.
//
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, date: new Timestamp(1341337661, 1)},
    {_id: 2, date: new Date(1341337661000)},
]));

res = coll.aggregate({
              $project: {
                  _id: 0,
                  dayOfYear: {$dayOfYear: '$date'},
                  dayOfMonth: {$dayOfMonth: "$date"},
                  dayOfWeek: {$dayOfWeek: "$date"},
              }
          })
          .toArray();
assert.eq(res[0], res[1]);

//
// Basic tests with timezones.
//
assert(coll.drop());
assert.commandWorked(coll.insert([
    {date: new Date("January 14, 2011"), timezone: "UTC"},
    {date: ISODate("1998-11-07T00:00:00Z"), timezone: "-0400"},
]));

res = coll.aggregate({
              $project: {
                  _id: 0,
                  dayOfYear: {$dayOfYear: {date: "$date", timezone: "$timezone"}},
                  dayOfMonth: {$dayOfMonth: {date: "$date", timezone: "$timezone"}},
                  dayOfWeek: {$dayOfWeek: {date: "$date", timezone: "$timezone"}},
              }
          })
          .toArray();
assertArrayEq({
    actual: res,
    expected: [
        {dayOfYear: 14, dayOfMonth: 14, dayOfWeek: 6},
        {dayOfYear: 310, dayOfMonth: 6, dayOfWeek: 6},
    ]
});

//
// Error Code tests.
//
let pipeline = {$project: {dayOfYear: {'$dayOfYear': {timezone: "$timezone"}}}};
assertErrorCode(coll, pipeline, 40539);

pipeline = {
    $project: {dayOfYear: {'$dayOfYear': {date: 42}}}
};
assertErrorCode(coll, pipeline, 16006);

pipeline = {
    $project: {date: {'$dayOfYear': {date: "$date", "timezone": 5}}}
};
assertErrorCode(coll, pipeline, 40533);

pipeline = {
    $project: {date: {'$dayOfYear': {date: "$date", "timezone": "DoesNot/Exist"}}}
};
assertErrorCode(coll, pipeline, 40485);
})();
