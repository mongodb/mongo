load("jstests/aggregation/extras/utils.js");  // For assertErrorCode

(function() {
    "use strict";

    const coll = db.dateFromParts;

    /* --------------------------------------------------------------------------------------- */
    /* Basic Sanity Checks */
    coll.drop();

    assert.writeOK(coll.insert([
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

    pipeline = {$project: {date: {'$dateFromParts': {"timezone": "$timezone"}}}};
    assertErrorCode(coll, pipeline, 40516);

    pipeline = {$project: {date: {'$dateFromParts': {year: false}}}};
    assertErrorCode(coll, pipeline, 40515);

    pipeline = {$project: {date: {'$dateFromParts': {year: 2012, "timezone": "DoesNot/Exist"}}}};
    assertErrorCode(coll, pipeline, 40485);

    pipeline = {$project: {date: {'$dateFromParts': {year: 2012, "timezone": 5}}}};
    assertErrorCode(coll, pipeline, 40517);

    /* --------------------------------------------------------------------------------------- */

    coll.drop();

    assert.writeOK(coll.insert([
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

    assert.writeOK(coll.insert([
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
        [{
           '$project':
               {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: "$isoDayOfWeek"}}}
        }],
    ];

    pipelines.forEach(function(pipeline) {
        assert.eq([{_id: 0, date: null}], coll.aggregate(pipeline).toArray(), tojson(pipeline));
    });

    pipeline = [{'$project': {date: {'$dateFromParts': {year: 2017, timezone: "$timezone"}}}}];
    assert.eq([{_id: 0, date: null}], coll.aggregate(pipeline).toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Testing whether it throws the right assert for uncoersable values */

    coll.drop();

    assert.writeOK(coll.insert([
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
        [{
           '$project':
               {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: "$falseValue"}}}
        }],
    ];

    pipelines.forEach(function(pipeline) {
        assertErrorCode(coll, pipeline, 40515, tojson(pipeline));
    });

    pipeline = [{'$project': {date: {'$dateFromParts': {year: 2017, timezone: "$falseValue"}}}}];
    assertErrorCode(coll, pipeline, 40517);

    /* --------------------------------------------------------------------------------------- */
    /* Testing whether it throws the right assert for uncoersable values */

    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, outOfRangeValue: 10002},
    ]));

    pipelines = [
        [{'$project': {date: {'$dateFromParts': {year: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, month: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, day: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, hour: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, minute: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, second: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, millisecond: "$outOfRangeValue"}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: "$outOfRangeValue"}}}}],
        [{
           '$project':
               {date: {'$dateFromParts': {isoWeekYear: 2017, isoWeek: "$outOfRangeValue"}}}
        }],
        [{
           '$project':
               {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: "$outOfRangeValue"}}}
        }],

        [{'$project': {date: {'$dateFromParts': {year: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 10000}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, month: 0}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, month: 13}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, day: 0}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, day: 32}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, hour: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, hour: 25}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, minute: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, minute: 60}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, second: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, second: 60}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, millisecond: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {year: 2017, millisecond: 1000}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: 10000}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoWeek: -1}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoWeek: 54}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: 0}}}}],
        [{'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, isoDayOfWeek: 8}}}}],
    ];

    pipelines.forEach(function(pipeline) {
        assertErrorCode(coll, pipeline, 40523, tojson(pipeline));
    });

    /* --------------------------------------------------------------------------------------- */
    /* Testing wrong arguments */

    coll.drop();

    assert.writeOK(coll.insert([
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
        {
          code: 40525,
          pipeline: {'$project': {date: {'$dateFromParts': {isoWeekYear: 2017, day: 17}}}}
        },
    ];

    pipelines.forEach(function(item) {
        assertErrorCode(coll, item.pipeline, item.code, tojson(pipeline));
    });

    /* --------------------------------------------------------------------------------------- */
    /* Testing wrong value (types) */

    coll.drop();

    assert.writeOK(coll.insert([
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

    assert.writeOK(coll.insert([
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

    assert.writeOK(coll.insert([
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

    assert.writeOK(coll.insert([
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
