/**
 * Tests for the $dateFromString expression with the optional 'onNull' parameter.
 */
(function() {
"use strict";

const onNullValue = ISODate("2017-07-04T11:56:02Z");
const coll = db.date_from_string_on_null;
coll.drop();

assert.commandWorked(coll.insert({_id: 0}));

// Test that the 'onNull' value is returned when the 'dateString' is nullish.
for (let inputDate of [null, undefined, "$missing"]) {
    assert.eq(
        [{_id: 0, date: onNullValue}],
        coll.aggregate(
                {$project: {date: {$dateFromString: {dateString: inputDate, onNull: onNullValue}}}})
            .toArray());
}

// Test that null is returned when the 'timezone' or 'format' is nullish, regardless of the
// 'onNull' value.
for (let nullishValue of [null, undefined, "$missing"]) {
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateFromString: {
                                  dateString: "2018-02-06T11:56:02Z",
                                  format: nullishValue,
                                  onNull: onNullValue
                              }
                          }
                      }
                  })
                  .toArray());
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateFromString: {
                                  dateString: "2018-02-06T11:56:02Z",
                                  timezone: nullishValue,
                                  onNull: onNullValue
                              }
                          }
                      }
                  })
                  .toArray());
}

// Test that 'onNull' can be any type, not just an ISODate.
for (let onNull of [{}, 5, "Not a date", null, undefined]) {
    assert.eq(
        [{_id: 0, date: onNull}],
        coll.aggregate(
                {$project: {date: {$dateFromString: {dateString: "$missing", onNull: onNull}}}})
            .toArray());
}
assert.eq(
    [{_id: 0}],
    coll.aggregate(
            {$project: {date: {$dateFromString: {dateString: "$missing", onNull: "$missing"}}}})
        .toArray());
})();
