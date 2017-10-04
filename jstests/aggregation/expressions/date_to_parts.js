load("jstests/aggregation/extras/utils.js");  // For assertErrorCode

(function() {
    "use strict";

    const coll = db.dateToParts;
    coll.drop();

    /* --------------------------------------------------------------------------------------- */
    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "UTC"},
        {_id: 1, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "Europe/London"},
        {_id: 2, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "America/New_York", iso: true},
        {_id: 3, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: "America/New_York", iso: false},
    ]));

    assert.eq(
        [
          {
            _id: 0,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 1,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 2,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
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
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}]).toArray());

    assert.eq(
        [
          {
            _id: 0,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 1,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 2,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 3,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz"}}}}])
            .toArray());

    assert.eq(
        [
          {
            _id: 0,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 1,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 2,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 3,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{
                $project: {
                    date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": false}}
                }
            }])
            .toArray());

    assert.eq(
        [
          {
            _id: 0,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 1,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 2,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 3,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{
                $project:
                    {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": true}}}
            }])
            .toArray());

    assert.eq(
        [
          {
            _id: 2,
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
          {
            _id: 3,
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 11,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([
                {$match: {iso: {$exists: true}}},
                {
                  $project: {
                      date: {
                          '$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": "$iso"}
                      }
                  }
                }
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Tests with timestamp */
    coll.drop();

    assert.writeOK(coll.insert([
        {
          _id: ObjectId("58c7cba47bbadf523cf2c313"),
          date: new ISODate("2017-06-19T15:13:25.713Z"),
          tz: "Europe/London"
        },
    ]));

    assert.eq(
        [
          {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 15,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}]).toArray());

    assert.eq(
        [
          {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz"}}}}])
            .toArray());

    assert.eq(
        [
          {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {
                year: 2017,
                month: 6,
                day: 19,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{
                $project: {
                    date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": false}}
                }
            }])
            .toArray());

    assert.eq(
        [
          {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date: {
                isoWeekYear: 2017,
                isoWeek: 25,
                isoDayOfWeek: 1,
                hour: 16,
                minute: 13,
                second: 25,
                millisecond: 713
            }
          },
        ],
        coll.aggregate([{
                $project:
                    {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": true}}}
            }])
            .toArray());

    assert.eq(
        [
          {
            _id: ObjectId("58c7cba47bbadf523cf2c313"),
            date:
                {year: 2017, month: 3, day: 14, hour: 10, minute: 53, second: 24, millisecond: 0}
          },
        ],
        coll.aggregate([{
                $project:
                    {date: {'$dateToParts': {date: "$_id", "timezone": "$tz", "iso8601": false}}}
            }])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: ISODate("2017-06-27T12:00:20Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: null},
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", timezone: "$tz"}}}}])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, date: ISODate("2017-06-27T12:00:20Z")},
    ]));

    assert.eq(
        [
          {_id: 0, date: null},
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date", iso8601: "$iso8601"}}}}])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0, tz: "Europe/London"},
    ]));

    assert.eq(
        [
          {_id: 0, date: null},
        ],
        coll.aggregate([{$project: {date: {'$dateToParts': {date: "$date"}}}}]).toArray());

    /* --------------------------------------------------------------------------------------- */

    let pipeline = {$project: {date: {'$dateToParts': {"timezone": "$tz"}}}};
    assertErrorCode(coll, pipeline, 40522);

    pipeline = {
        $project: {date: {'$dateToParts': {date: "$date", "timezone": "$tz", "iso8601": 5}}}
    };
    assertErrorCode(coll, pipeline, 40521);

    pipeline = {$project: {date: {'$dateToParts': {date: 42}}}};
    assertErrorCode(coll, pipeline, 16006);

    pipeline = {$project: {date: {'$dateToParts': {date: "$date", "timezone": 5}}}};
    assertErrorCode(coll, pipeline, 40517);

    pipeline = {$project: {date: {'$dateToParts': {date: "$date", "timezone": "DoesNot/Exist"}}}};
    assertErrorCode(coll, pipeline, 40485);

})();
