/**
 * Tests for the $dateFromString expression with the optional 'onError' parameter.
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrMsgContains.

    const onErrorValue = ISODate("2017-07-04T11:56:02Z");
    const coll = db.date_from_string_on_error;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0}));

    // Test that the 'onError' value is returned when 'dateString' is not a valid date/time.
    for (let inputDate of["July 4th",
                          "12:50:53",
                          "2017",
                          "60.Monday1770/06:59",
                          "Not even close",
                          "July 4th, 10000"]) {
        assert.eq(
            [{_id: 0, date: onErrorValue}],
            coll.aggregate({
                    $project:
                        {date: {$dateFromString: {dateString: inputDate, onError: onErrorValue}}}
                })
                .toArray());
    }

    // Test that the 'onError' value is returned when 'dateString' is not a string.
    for (let inputDate of[5, {year: 2018, month: 2, day: 5}, ["2018-02-05"]]) {
        assert.eq(
            [{_id: 0, date: onErrorValue}],
            coll.aggregate({
                    $project:
                        {date: {$dateFromString: {dateString: inputDate, onError: onErrorValue}}}
                })
                .toArray());
    }

    // Test that the 'onError' value is ignored when 'dateString' is nullish.
    for (let inputDate of[null, undefined, "$missing"]) {
        assert.eq(
            [{_id: 0, date: null}],
            coll.aggregate({
                    $project:
                        {date: {$dateFromString: {dateString: inputDate, onError: onErrorValue}}}
                })
                .toArray());
    }

    // Test that the 'onError' value is returned for unmatched format strings.
    for (let inputFormat of["%Y", "%Y-%m-%dT%H", "Y-m-d"]) {
        assert.eq([{_id: 0, date: onErrorValue}],
                  coll.aggregate({
                          $project: {
                              date: {
                                  $dateFromString: {
                                      dateString: "2018-02-06",
                                      format: inputFormat,
                                      onError: onErrorValue
                                  }
                              }
                          }
                      })
                      .toArray());
    }

    // Test that null is returned when the 'timezone' or 'format' is nullish, regardless of the
    // 'onError' value.
    for (let nullishValue of[null, undefined, "$missing"]) {
        assert.eq([{_id: 0, date: null}],
                  coll.aggregate({
                          $project: {
                              date: {
                                  $dateFromString: {
                                      dateString: "2018-02-06T11:56:02Z",
                                      format: nullishValue,
                                      onError: onErrorValue
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
                                      onError: onErrorValue
                                  }
                              }
                          }
                      })
                      .toArray());
    }

    // Test that onError is returned when the input is not a string and other parameters are
    // nullish.
    assert.eq(
        [{_id: 0, date: onErrorValue}],
        coll.aggregate({
                $project: {
                    date: {$dateFromString: {dateString: 5, format: null, onError: onErrorValue}}
                }
            })
            .toArray());
    assert.eq([{_id: 0, date: onErrorValue}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateFromString:
                                  {dateString: 5, timezone: "$missing", onError: onErrorValue}
                          }
                      }
                  })
                  .toArray());

    // Test that onError is ignored when the input is an invalid string and other parameters are
    // nullish.
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateFromString: {
                                  dateString: "Invalid date string",
                                  format: null,
                                  onError: onErrorValue
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
                                  dateString: "Invalid date string",
                                  timezone: "$missing",
                                  onError: onErrorValue
                              }
                          }
                      }
                  })
                  .toArray());

    // Test that 'onError' can be any type, not just an ISODate.
    for (let onError of[{}, 5, "Not a date", null, undefined]) {
        assert.eq(
            [{_id: 0, date: onError}],
            coll.aggregate({
                    $project: {date: {$dateFromString: {dateString: "invalid", onError: onError}}}
                })
                .toArray());
    }
    // Test that a missing 'onError' value results in no output field when used within a $project
    // stage.
    assert.eq(
        [{_id: 0}],
        coll.aggregate(
                {$project: {date: {$dateFromString: {dateString: "invalid", onError: "$missing"}}}})
            .toArray());

    // Test that 'onError' is ignored when the 'format' is invalid.
    assertErrCodeAndErrMsgContains(
        coll,
        [{
           $project: {
               date: {
                   $dateFromString: {dateString: "4/26/1992", format: 5, onError: onErrorValue}
               }
           }
        }],
        40684,
        "$dateFromString requires that 'format' be a string");

    assertErrCodeAndErrMsgContains(
        coll,
        [{
           $project: {
               date: {
                   $dateFromString:
                       {dateString: "4/26/1992", format: "%n", onError: onErrorValue}
               }
           }
        }],
        18536,
        "Invalid format character '%n' in format string");
})();
