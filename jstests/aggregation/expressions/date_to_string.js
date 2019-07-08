load("jstests/aggregation/extras/utils.js");  // For assertErrorCode

(function() {
    "use strict";

    const coll = db.date_to_string;
    coll.drop();

    /* --------------------------------------------------------------------------------------- */

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "UTC"},
        {_id: 1, date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "Europe/London"},
        {_id: 2, date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "America/New_York"},
        {_id: 3, date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "Australia/Eucla"},
        {_id: 4, date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "Asia/Kathmandu"},
        {_id: 5, date: new ISODate("1935-07-10T11:36:37.133Z"), tz: "Europe/Amsterdam"},
        {_id: 6, date: new ISODate("1900-07-10T11:41:22.418Z"), tz: "America/Caracas"},
    ]));

    assert.eq(
        [
          {_id: 0, date: "2017-07-04 14:56:42 +0000 (0 minutes)"},
          {_id: 1, date: "2017-07-04 15:56:42 +0100 (60 minutes)"},
          {_id: 2, date: "2017-07-04 10:56:42 -0400 (-240 minutes)"},
          {_id: 3, date: "2017-07-04 23:41:42 +0845 (525 minutes)"},
          {_id: 4, date: "2017-07-04 20:41:42 +0545 (345 minutes)"},
          {_id: 5, date: "1935-07-10 12:56:09 +0119 (79 minutes)"},
          {_id: 6, date: "1900-07-10 07:13:42 -0427 (-267 minutes)"},
        ],
        coll.aggregate([
                {
                  $project: {
                      date: {
                          $dateToString: {
                              format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)",
                              date: "$date",
                              timezone: "$tz"
                          }
                      }
                  }
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-01-04T15:08:51.911Z")},
        {_id: 1, date: new ISODate("2017-07-04T15:09:12.911Z")},
        {_id: 2, date: new ISODate("2017-12-04T15:09:14.911Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: "2017-01-04 10:08:51 -0500 (-300 minutes)"},
          {_id: 1, date: "2017-07-04 11:09:12 -0400 (-240 minutes)"},
          {_id: 2, date: "2017-12-04 10:09:14 -0500 (-300 minutes)"},
        ],
        coll.aggregate([
                {
                  $project: {
                      date: {
                          $dateToString: {
                              format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)",
                              date: "$date",
                              timezone: "America/New_York"
                          }
                      }
                  }
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-01-04T15:08:51.911Z")},
        {_id: 1, date: new ISODate("2017-07-04T15:09:12.911Z")},
        {_id: 2, date: new ISODate("2017-12-04T15:09:14.911Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: "2017-01-04 15:08:51 +0000 (0 minutes)"},
          {_id: 1, date: "2017-07-04 15:09:12 +0000 (0 minutes)"},
          {_id: 2, date: "2017-12-04 15:09:14 +0000 (0 minutes)"},
        ],
        coll.aggregate([
                {
                  $project: {
                      date: {
                          $dateToString:
                              {format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)", date: "$date"}
                      }
                  }
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-01-01T15:08:51.911Z")},
        {_id: 1, date: new ISODate("2017-07-04T15:09:12.911Z")},
        {_id: 2, date: new ISODate("2017-12-04T15:09:14.911Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: "Natural: 2017-W1-01, ISO: 2016-W7-52"},
          {_id: 1, date: "Natural: 2017-W3-27, ISO: 2017-W2-27"},
          {_id: 2, date: "Natural: 2017-W2-49, ISO: 2017-W1-49"},
        ],
        coll.aggregate([
                {
                  $project: {
                      date: {
                          $dateToString:
                              {format: "Natural: %Y-W%w-%U, ISO: %G-W%u-%V", date: "$date"}
                      }
                  }
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Test that missing expressions, turn into BSON null values */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-01-04T15:08:51.911Z")},
        {_id: 1, date: new ISODate("2017-01-04T15:08:51.911Z"), timezone: null},
        {_id: 2, date: new ISODate("2017-01-04T15:08:51.911Z"), timezone: undefined},
        {_id: 3, timezone: "Europe/Oslo"},
        {_id: 4, date: null, timezone: "Europe/Oslo"},
        {_id: 5, date: undefined, timezone: "Europe/Oslo"},
    ]));

    assert.eq(
        [
          {_id: 0, date: null},
          {_id: 1, date: null},
          {_id: 2, date: null},
          {_id: 3, date: null},
          {_id: 4, date: null},
          {_id: 5, date: null},
        ],
        coll.aggregate([
                {
                  $project: {
                      date: {
                          $dateToString: {
                              format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)",
                              date: "$date",
                              timezone: "$timezone"
                          }
                      }
                  }
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Test that the default format is "%Y-%m-%dT%H:%M:%S.%LZ" if none specified. */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-01-04T15:08:51.911Z")},
        {_id: 1, date: new ISODate("2017-07-04T15:09:12.911Z")},
        {_id: 2, date: new ISODate("2017-12-04T15:09:14.911Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: "2017-01-04T10:08:51.911Z"},
          {_id: 1, date: "2017-07-04T11:09:12.911Z"},
          {_id: 2, date: "2017-12-04T10:09:14.911Z"},
        ],
        coll.aggregate([
                {$project: {date: {$dateToString: {date: "$date", timezone: "America/New_York"}}}},
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Test that null is returned when 'format' evaluates to nullish. */
    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));

    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateToString: {
                                  date: new ISODate("2017-01-04T15:08:51.911Z"),
                                  format: null,
                              }
                          }
                      }
                  })
                  .toArray());
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateToString: {
                                  date: new ISODate("2017-01-04T15:08:51.911Z"),
                                  format: undefined,
                              }
                          }
                      }
                  })
                  .toArray());
    assert.eq([{_id: 0, date: null}],
              coll.aggregate({
                      $project: {
                          date: {
                              $dateToString: {
                                  date: new ISODate("2017-01-04T15:08:51.911Z"),
                                  format: "$missing",
                              }
                          }
                      }
                  })
                  .toArray());

    /* --------------------------------------------------------------------------------------- */

    let pipeline = [{
        $project:
            {date: {$dateToString: {date: new ISODate("2017-01-04T15:08:51.911Z"), format: 5}}}
    }];
    assertErrCodeAndErrMsgContains(
        coll, pipeline, 18533, "$dateToString requires that 'format' be a string");

    pipeline =
        [{$project: {date: {$dateToString: {format: "%Y-%m-%d %H:%M:%S", timezone: "$tz"}}}}];
    assertErrCodeAndErrMsgContains(
        coll, pipeline, 18628, "Missing 'date' parameter to $dateToString");

    pipeline = [{
        $project: {
            date: {
                $dateToString: {
                    date: new ISODate("2017-01-04T15:08:51.911Z"),
                    format: "%Y-%m-%d %H:%M:%S",
                    timezone: 5
                }
            }
        }
    }];
    assertErrCodeAndErrMsgContains(coll, pipeline, 40517, "timezone must evaluate to a string");

    pipeline = [{$project: {date: {$dateToString: {format: "%Y-%m-%d %H:%M:%S", date: 42}}}}];
    assertErrCodeAndErrMsgContains(
        coll, pipeline, 16006, "can't convert from BSON type double to Date");

    pipeline = [{
        $project: {
            date: {
                $dateToString: {
                    date: new ISODate("2017-01-04T15:08:51.911Z"),
                    format: "%Y-%m-%d %H:%M:%S",
                    timezone: "DoesNotExist"
                }
            }
        }
    }];
    assertErrCodeAndErrMsgContains(coll, pipeline, 40485, "unrecognized time zone identifier");

    pipeline = [{
        $project:
            {date: {$dateToString: {date: new ISODate("2017-01-04T15:08:51.911Z"), format: "%"}}}
    }];
    assertErrCodeAndErrMsgContains(coll, pipeline, 18535, "Unmatched '%' at end of format string");

    // Fails for unknown format specifier.
    pipeline = [{
        $project: {
            date: {$dateToString: {date: new ISODate("2017-01-04T15:08:51.911Z"), format: "%n"}}
        }
    }];
    assertErrCodeAndErrMsgContains(
        coll, pipeline, 18536, "Invalid format character '%n' in format string");
})();
