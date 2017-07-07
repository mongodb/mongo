// Basic tests for using date expressions with time zone arguments.
(function() {
    "use strict";

    const coll = db.date_expressions_with_time_zones;
    coll.drop();

    assert.writeOK(coll.insert([
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
})();
