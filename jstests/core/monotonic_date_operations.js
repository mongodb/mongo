/**
 * Tests that date expressions remain monotonic despite timezone changes.
 */

(function() {
"use strict";

const coll = db.monotonic_date_operations;

const runTest = function(documents, pipeline, expectedDocs) {
    coll.drop();
    assert.commandWorked(coll.insertMany(documents));
    pipeline.push({$sort: {_id: 1}});
    assert.eq(coll.aggregate(pipeline).toArray(), expectedDocs);
};

const dateFormat = "%Y-%m-%dT%H:%M:%S.%L%z";

// New York DST fall switch
runTest(
    [
        {_id: 0, time: ISODate("2022-11-06T03:00:00Z")},  // 2022-11-05T23:00:00 in New York
        {_id: 1, time: ISODate("2022-11-06T05:00:00Z")},
        {_id: 2, time: ISODate("2022-11-06T05:30:00Z")},
        {_id: 3, time: ISODate("2022-11-06T06:00:00Z")},  // Moment of DST switch
        {_id: 4, time: ISODate("2022-11-06T06:30:00Z")},
        {_id: 5, time: ISODate("2022-11-06T07:00:00Z")},
    ],
    [{
        $project: {
            localTime: {
                $dateToString: {
                    date: "$time",
                    format: dateFormat,
                    timezone: "America/New_York",
                }
            },
            diffMinutes: {
                $dateDiff: {
                    startDate: ISODate("2022-11-06T04:00:00Z"),
                    endDate: "$time",
                    unit: "minute",
                    timezone: "America/New_York",
                }
            },
            diffDays: {
                $dateDiff: {
                    startDate: ISODate("2022-11-06T04:00:00Z"),
                    endDate: "$time",
                    unit: "day",
                    timezone: "America/New_York",
                }
            }
        }
    }],
    [
        {
            _id: 0,
            localTime: "2022-11-05T23:00:00.000-0400",
            diffMinutes: NumberLong(-60),
            diffDays: NumberLong(-1)
        },
        {
            _id: 1,
            localTime: "2022-11-06T01:00:00.000-0400",
            diffMinutes: NumberLong(60),
            diffDays: NumberLong(0)
        },
        {
            _id: 2,
            localTime: "2022-11-06T01:30:00.000-0400",
            diffMinutes: NumberLong(90),
            diffDays: NumberLong(0)
        },
        {
            _id: 3,
            localTime: "2022-11-06T01:00:00.000-0500",
            diffMinutes: NumberLong(120),
            diffDays: NumberLong(0)
        },
        {
            _id: 4,
            localTime: "2022-11-06T01:30:00.000-0500",
            diffMinutes: NumberLong(150),
            diffDays: NumberLong(0)
        },
        {
            _id: 5,
            localTime: "2022-11-06T02:00:00.000-0500",
            diffMinutes: NumberLong(180),
            diffDays: NumberLong(0)
        }
    ]);

// New York DST spring switch
runTest(
    [
        {_id: 0, time: ISODate("2022-03-13T06:00:00Z")},  // 2022-03-13T01:00:00 in New York
        {_id: 1, time: ISODate("2022-03-13T06:30:00Z")},
        {_id: 2, time: ISODate("2022-03-13T07:00:00Z")},  // Moment of DST switch
        {_id: 3, time: ISODate("2022-03-13T07:30:00Z")},
        {_id: 4, time: ISODate("2022-03-13T08:00:00Z")},
    ],
    [{
        $project: {
            localTime: {
                $dateToString: {
                    date: "$time",
                    format: dateFormat,
                    timezone: "America/New_York",
                }
            },
            diffMinutes: {
                $dateDiff: {
                    startDate: ISODate("2022-03-13T00:00:00Z"),
                    endDate: "$time",
                    unit: "minute",
                    timezone: "America/New_York",
                }
            },
            diffDays: {
                $dateDiff: {
                    startDate: ISODate("2022-03-13T00:00:00Z"),
                    endDate: "$time",
                    unit: "day",
                    timezone: "America/New_York",
                }
            }
        }
    }],
    [
        {
            _id: 0,
            localTime: "2022-03-13T01:00:00.000-0500",
            diffMinutes: NumberLong(360),
            diffDays: NumberLong(1)
        },
        {
            _id: 1,
            localTime: "2022-03-13T01:30:00.000-0500",
            diffMinutes: NumberLong(390),
            diffDays: NumberLong(1)
        },
        {
            _id: 2,
            localTime: "2022-03-13T03:00:00.000-0400",
            diffMinutes: NumberLong(420),
            diffDays: NumberLong(1)
        },
        {
            _id: 3,
            localTime: "2022-03-13T03:30:00.000-0400",
            diffMinutes: NumberLong(450),
            diffDays: NumberLong(1)
        },
        {
            _id: 4,
            localTime: "2022-03-13T04:00:00.000-0400",
            diffMinutes: NumberLong(480),
            diffDays: NumberLong(1)
        },
    ]);

// Samoa day skip
runTest(
    [
        {_id: 0, time: ISODate("2011-12-29T10:00:00Z")},
        {_id: 1, time: ISODate("2011-12-30T09:00:00Z")},  // 2011-12-29T23:00:00 in Apia, Samoa
        {_id: 2, time: ISODate("2011-12-30T09:30:00Z")},
        {_id: 3, time: ISODate("2011-12-30T10:00:00Z")},  // Day skip moment
        {_id: 4, time: ISODate("2011-12-30T10:30:00Z")},
        {_id: 5, time: ISODate("2011-12-30T11:00:00Z")},
        {_id: 6, time: ISODate("2011-12-30T10:00:00Z")},
    ],
    [{
        $project: {
            localTime: {
                $dateToString: {
                    date: "$time",
                    format: dateFormat,
                    timezone: "Pacific/Apia",
                }
            },
            diffMinutes: {
                $dateDiff: {
                    startDate: ISODate("2011-12-28T10:00:00Z"),
                    endDate: "$time",
                    unit: "minute",
                    timezone: "Pacific/Apia",
                }
            },
            diffDays: {
                $dateDiff: {
                    startDate: ISODate("2011-12-28T10:00:00Z"),
                    endDate: "$time",
                    unit: "day",
                    timezone: "Pacific/Apia",
                }
            }
        }
    }],
    [
        {
            _id: 0,
            localTime: "2011-12-29T00:00:00.000-1000",
            diffMinutes: NumberLong(1440),
            diffDays: NumberLong(1)
        },
        {
            _id: 1,
            localTime: "2011-12-29T23:00:00.000-1000",
            diffMinutes: NumberLong(2820),
            diffDays: NumberLong(1)
        },
        {
            _id: 2,
            localTime: "2011-12-29T23:30:00.000-1000",
            diffMinutes: NumberLong(2850),
            diffDays: NumberLong(1)
        },
        {
            _id: 3,
            localTime: "2011-12-31T00:00:00.000+1400",
            diffMinutes: NumberLong(2880),
            diffDays: NumberLong(3)
        },
        {
            _id: 4,
            localTime: "2011-12-31T00:30:00.000+1400",
            diffMinutes: NumberLong(2910),
            diffDays: NumberLong(3)
        },
        {
            _id: 5,
            localTime: "2011-12-31T01:00:00.000+1400",
            diffMinutes: NumberLong(2940),
            diffDays: NumberLong(3)
        },
        {
            _id: 6,
            localTime: "2011-12-31T00:00:00.000+1400",
            diffMinutes: NumberLong(2880),
            diffDays: NumberLong(3)
        },
    ]);

// Sao Paulo, Brazil DST fall switch
runTest(
    [
        {
            _id: 0,
            time: ISODate("2018-11-04T02:00:00Z")
        },  // 2018-11-03T23:00:00 in Sao Paulo, Brazil
        {_id: 1, time: ISODate("2018-11-04T02:30:00Z")},
        {_id: 2, time: ISODate("2018-11-04T03:00:00Z")},  // Moment of DST switch
        {_id: 3, time: ISODate("2018-11-04T03:30:00Z")},
        {_id: 4, time: ISODate("2018-11-04T04:00:00Z")},
        {_id: 5, time: ISODate("2018-11-04T04:30:00Z")},
    ],
    [{
        $project: {
            localTime: {
                $dateToString: {
                    date: "$time",
                    format: dateFormat,
                    timezone: "America/Sao_Paulo",
                }
            },
            diffMinutes: {
                $dateDiff: {
                    startDate: ISODate("2018-11-04T02:00:00Z"),
                    endDate: "$time",
                    unit: "minute",
                    timezone: "America/Sao_Paulo",
                }
            },
            diffDays: {
                $dateDiff: {
                    startDate: ISODate("2018-11-04T02:00:00Z"),
                    endDate: "$time",
                    unit: "day",
                    timezone: "America/Sao_Paulo",
                }
            }
        }
    }],
    [
        {
            _id: 0,
            localTime: "2018-11-03T23:00:00.000-0300",
            diffMinutes: NumberLong(0),
            diffDays: NumberLong(0)
        },
        {
            _id: 1,
            localTime: "2018-11-03T23:30:00.000-0300",
            diffMinutes: NumberLong(30),
            diffDays: NumberLong(0)
        },
        {
            _id: 2,
            localTime: "2018-11-04T01:00:00.000-0200",
            diffMinutes: NumberLong(60),
            diffDays: NumberLong(1)
        },
        {
            _id: 3,
            localTime: "2018-11-04T01:30:00.000-0200",
            diffMinutes: NumberLong(90),
            diffDays: NumberLong(1)
        },
        {
            _id: 4,
            localTime: "2018-11-04T02:00:00.000-0200",
            diffMinutes: NumberLong(120),
            diffDays: NumberLong(1)
        },
        {
            _id: 5,
            localTime: "2018-11-04T02:30:00.000-0200",
            diffMinutes: NumberLong(150),
            diffDays: NumberLong(1)
        },
    ]);

// Sao Paulo, Brazil DST sprint switch
runTest(
    [
        {_id: 0, time: ISODate("2018-02-18T00:30:00Z")},  // 2018-02-17T22:30:00 in Sao Paulo, Brazi
        {_id: 1, time: ISODate("2018-02-18T01:00:00Z")},
        {_id: 2, time: ISODate("2018-02-18T01:30:00Z")},
        {_id: 3, time: ISODate("2018-02-18T02:00:00Z")},  // Moment of DST switch
        {_id: 4, time: ISODate("2018-02-18T02:30:00Z")},
        {_id: 5, time: ISODate("2018-02-18T03:00:00Z")},
        {_id: 6, time: ISODate("2018-02-18T03:30:00Z")},
    ],
    [{
        $project: {
            localTime: {
                $dateToString: {
                    date: "$time",
                    format: dateFormat,
                    timezone: "America/Sao_Paulo",
                }
            },
            diffMinutes: {
                $dateDiff: {
                    startDate: ISODate("2018-02-18T00:30:00Z"),
                    endDate: "$time",
                    unit: "minute",
                    timezone: "America/Sao_Paulo",
                }
            },
            diffDays: {
                $dateDiff: {
                    startDate: ISODate("2018-02-18T00:30:00Z"),
                    endDate: "$time",
                    unit: "day",
                    timezone: "America/Sao_Paulo",
                }
            }
        }
    }],
    [
        {
            _id: 0,
            localTime: "2018-02-17T22:30:00.000-0200",
            diffMinutes: NumberLong(0),
            diffDays: NumberLong(0)
        },
        {
            _id: 1,
            localTime: "2018-02-17T23:00:00.000-0200",
            diffMinutes: NumberLong(30),
            diffDays: NumberLong(0)
        },
        {
            _id: 2,
            localTime: "2018-02-17T23:30:00.000-0200",
            diffMinutes: NumberLong(60),
            diffDays: NumberLong(0)
        },
        {
            _id: 3,
            localTime: "2018-02-17T23:00:00.000-0300",
            diffMinutes: NumberLong(90),
            diffDays: NumberLong(0)
        },
        {
            _id: 4,
            localTime: "2018-02-17T23:30:00.000-0300",
            diffMinutes: NumberLong(120),
            diffDays: NumberLong(0)
        },
        {
            _id: 5,
            localTime: "2018-02-18T00:00:00.000-0300",
            diffMinutes: NumberLong(150),
            diffDays: NumberLong(1)
        },
        {
            _id: 6,
            localTime: "2018-02-18T00:30:00.000-0300",
            diffMinutes: NumberLong(180),
            diffDays: NumberLong(1)
        }
    ]);
})();
