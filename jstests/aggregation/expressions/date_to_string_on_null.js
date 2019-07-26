/**
 * Tests for the $dateToString expression with the optional 'onNull' parameter.
 */
(function() {
"use strict";

const onNullValue = ISODate("2017-07-04T11:56:02Z");
const coll = db.date_to_string_on_null;
coll.drop();

assert.writeOK(coll.insert({_id: 0}));

for (let nullishValue of [null, undefined, "$missing"]) {
    // Test that the 'onNull' value is returned when the 'date' is nullish.
    assert.eq(
        [{_id: 0, date: onNullValue}],
        coll.aggregate({
                $project: {
                    date: {
                        $dateToString:
                            {date: nullishValue, format: "%Y-%m-%d %H:%M:%S", onNull: onNullValue}
                    }
                }
            })
            .toArray());

    // Test that null is returned when the 'timezone' is nullish, regardless of the 'onNull'
    // value.
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateToString: {
                                  date: "2018-02-06T11:56:02Z",
                                  format: "%Y-%m-%d %H:%M:%S",
                                  timezone: nullishValue,
                                  onNull: onNullValue
                              }
                          }
                      }
                  })
                  .toArray());
}

// Test that 'onNull' can be any type, not just an ISODate.
for (let onNullValue of [{}, 5, "Not a date", null, undefined]) {
    assert.eq(
        [{_id: 0, date: onNullValue}],
        coll.aggregate({
                $project: {
                    date: {
                        $dateToString:
                            {date: "$missing", format: "%Y-%m-%d %H:%M:%S", onNull: onNullValue}
                    }
                }
            })
            .toArray());
}

// Test that 'onNull' can be missing, resulting in no output field when used within a $project
// stage.
assert.eq([{_id: 0}],
          coll.aggregate({
                  $project: {
                      date: {
                          $dateToString:
                              {date: "$missing", format: "%Y-%m-%d %H:%M:%S", onNull: "$missing"}
                      }
                  }
              })
              .toArray());
})();
