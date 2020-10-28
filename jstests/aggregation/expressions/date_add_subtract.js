/**
 * Tests for $dateAdd and $dateSubtract expressions.
 * @tags: [
 *    sbe_incompatible,
 *    requires_fcv_49
 * ]
 */

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.dateAddSubtract;
coll.drop();

assert.commandWorked(coll.insert(
    [{_id: 1, date: ISODate("2020-12-31T12:10:05"), unit: "month", timezone: "Europe/Paris"}]));

// $dateAdd
assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-01T12:10:05Z")}],
    coll.aggregate([{
            $project: {
                newDate:
                    {$dateAdd: {startDate: ISODate("2020-11-30T12:10:05Z"), unit: "day", amount: 1}}
            }
        }])
        .toArray());
// Tests when startDate and result date cross the DST time change in a timezone.
assert.eq([{_id: 1, newDate: ISODate("2020-10-25T18:10:00Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateAdd: {
                              startDate: ISODate("2020-10-24T18:10:00Z"),
                              unit: "hour",
                              amount: 24,
                              timezone: "Europe/Paris"
                          }
                      }
                  }
              }])
              .toArray());
// When adding units of day both the startDate and the result represent 20:10:00 in Europe/Paris.
// The two dates have different offsets from UTC due to the change in daylight savings time.
assert.eq([{_id: 1, newDate: ISODate("2020-10-25T19:10:00Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateAdd: {
                              startDate: ISODate("2020-10-24T18:10:00Z"),
                              unit: "day",
                              amount: 1,
                              timezone: "Europe/Paris"
                          }
                      }
                  }
              }])
              .toArray());

// Test last day adjustment in a timezone.
assert.eq([{_id: 1, newDate: ISODate("2021-02-27T23:30:00Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateAdd: {
                              startDate: ISODate("2021-01-30T23:30:00Z"),
                              unit: "month",
                              amount: 1,
                              timezone: "Europe/Paris"
                          }
                      }
                  }
              }])
              .toArray());

// Test that adding with a null argument (non-existing field) results in null.
assert.eq(
    [{_id: 1, newDate: null}],
    coll.aggregate(
            [{$project: {newDate: {$dateAdd: {startDate: "$dateSent", unit: "day", amount: 1}}}}])
        .toArray());

assert.eq([{_id: 1, newDate: ISODate("2021-01-01T12:10:05Z")}],
          coll.aggregate(
                  [{$project: {newDate: {$dateAdd: {startDate: "$date", unit: "day", amount: 1}}}}])
              .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2021-01-07T12:10:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateAdd: {startDate: "$date", unit: "week", amount: 1}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T14:10:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateAdd: {startDate: "$date", unit: "hour", amount: 2}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T13:00:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateAdd: {startDate: "$date", unit: "minute", amount: 50}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T12:10:06.05Z")}],
    coll.aggregate([{
            $project: {newDate: {$dateAdd: {startDate: "$date", unit: "millisecond", amount: 1050}}}
        }])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2021-01-31T12:10:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateAdd: {startDate: "$date", unit: "$unit", amount: 1}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T14:10:05Z")}],
    coll.aggregate([{
            $project: {
                newDate:
                    {$dateAdd: {startDate: "$date", unit: "hour", amount: 2, timezone: "$timezone"}}
            }
        }])
        .toArray());

// $dateSubtract
assert.eq(
    [{_id: 1, newDate: ISODate("2019-12-31T12:10:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "year", amount: 1}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-06-30T12:10:05Z")}],
    coll.aggregate([
            {$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "quarter", amount: 2}}}}
        ])
        .toArray());

// Tests using the document fields for unit and timezone arguments
assert.eq(
    [{_id: 1, newDate: ISODate("2020-11-30T12:10:05Z")}],
    coll.aggregate([
            {$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "$unit", amount: 1}}}}
        ])
        .toArray());

assert.eq([{_id: 1, newDate: ISODate("2020-09-30T11:10:05Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateSubtract:
                              {startDate: "$date", unit: "$unit", amount: 3, timezone: "$timezone"}
                      }
                  }
              }])
              .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-16T12:10:05Z")}],
    coll.aggregate(
            [{$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "day", amount: 15}}}}])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-29T12:10:05Z")}],
    coll.aggregate([
            {$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "hour", amount: 48}}}}
        ])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T11:55:05Z")}],
    coll.aggregate([
            {$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "minute", amount: 15}}}}
        ])
        .toArray());

assert.eq(
    [{_id: 1, newDate: ISODate("2020-12-31T12:08:00Z")}],
    coll.aggregate([{
            $project: {newDate: {$dateSubtract: {startDate: "$date", unit: "second", amount: 125}}}
        }])
        .toArray());

// Test for the last day adjustment.
assert.eq([{_id: 1, newDate: ISODate("2020-11-30T03:00:00Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateSubtract: {
                              startDate: ISODate("2021-01-31T03:00:00Z"),
                              unit: "month",
                              amount: 2,
                          }
                      }
                  }
              }])
              .toArray());

// Test last day adjustment in New York timezone.
assert.eq([{_id: 1, newDate: ISODate("2020-12-01T03:00:00Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateSubtract: {
                              startDate: ISODate("2021-01-31T03:00:00Z"),
                              unit: "month",
                              amount: 2,
                              timezone: "America/New_York"
                          }
                      }
                  }
              }])
              .toArray());

// Tests combining $dateAdd and $dateSubtract.
assert.eq([{_id: 1, newDate: ISODate("2020-12-31T12:10:05Z")}],
          coll.aggregate([{
                  $project: {
                      newDate: {
                          $dateSubtract: {
                              startDate: {$dateAdd: {startDate: "$date", unit: "hour", amount: 2}},
                              unit: "hour",
                              amount: 2
                          }
                      }
                  }
              }])
              .toArray());

assert.eq(
    coll.aggregate([
            {$project: {newDate: {$dateSubtract: {startDate: "$date", unit: "month", amount: 1}}}}
        ])
        .toArray(),
    coll.aggregate([{
            $project: {
                newDate: {
                    $dateAdd:
                        {startDate: ISODate("2020-11-30T12:10:00Z"), unit: "second", amount: 5}
                }
            }
        }])
        .toArray());

// Tests for error codes.
let pipeline = {$project: {newDate: {$dateAdd: {unit: "second", amount: 120}}}};
assertErrorCode(coll, pipeline, 5166402);

pipeline = {
    $project: {newDate: {$dateSubtract: {startDate: "$date", amount: 1}}}
};
assertErrorCode(coll, pipeline, 5166402);

pipeline = {
    $project: {newDate: {$dateAdd: {startDate: "$date", units: "day", amount: 1}}}
};
assertErrorCode(coll, pipeline, 5166401);

pipeline = {
    $project: {newDate: {$dateSubtract: {startDate: "myBirthDate", unit: "year", amount: 10}}}
};
assertErrorCode(coll, pipeline, 5166403);

pipeline = {
    $project: {newDate: {$dateAdd: {startDate: "$date", unit: 1, amount: 10}}}
};
assertErrorCode(coll, pipeline, 5166404);

pipeline = {
    $project: {newDate: {$dateAdd: {startDate: "$date", unit: "epoch", amount: 10}}}
};
assertErrorCode(coll, pipeline, ErrorCodes.FailedToParse);

pipeline = {
    $project: {newDate: {$dateSubtract: {startDate: "$date", unit: "year", amount: 1.001}}}
};
assertErrorCode(coll, pipeline, 5166405);

pipeline = {
    $project: {
        newDate: {$dateSubtract: {startDate: "$date", unit: "year", amount: 1, timezone: "Unknown"}}
    }
};
assertErrorCode(coll, pipeline, 40485);
})();
