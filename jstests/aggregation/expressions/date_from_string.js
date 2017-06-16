load("jstests/aggregation/extras/utils.js");  // For assertErrorCode

(function() {
    "use strict";

    const coll = db.date_from_string;

    /* --------------------------------------------------------------------------------------- */
    /* Normal format tests. */

    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));

    let testCases = [
        {expect: "2017-07-04T11:56:02Z", inputString: "2017-07-04T11:56:02Z"},
        {expect: "2017-07-04T11:56:02.813Z", inputString: "2017-07-04T11:56:02.813Z"},
        {expect: "2017-07-04T11:56:02.810Z", inputString: "2017-07-04T11:56:02.81Z"},
        {expect: "2017-07-04T11:56:02.800Z", inputString: "2017-07-04T11:56:02.8Z"},
        {expect: "2017-07-04T11:56:02Z", inputString: "2017-07-04T11:56.02"},
        {expect: "2017-07-04T11:56:02.813Z", inputString: "2017-07-04T11:56.02.813"},
        {expect: "2017-07-04T11:56:02.810Z", inputString: "2017-07-04T11:56.02.81"},
        {expect: "2017-07-04T11:56:02.800Z", inputString: "2017-07-04T11:56.02.8"},
    ];
    testCases.forEach(function(testCase) {
        assert.eq(
            [{_id: 0, date: ISODate(testCase.expect)}],
            coll.aggregate(
                    {$project: {date: {'$dateFromString': {"dateString": testCase.inputString}}}})
                .toArray(),
            tojson(testCase));
    });

    /* --------------------------------------------------------------------------------------- */
    /* Normal format tests from data. */

    coll.drop();
    assert.writeOK(coll.insert([
        {_id: 0, dateString: "2017-07-06T12:35:37Z"},
        {_id: 1, dateString: "2017-07-06T12:35:37.513Z"},
        {_id: 2, dateString: "2017-07-06T12:35:37"},
        {_id: 3, dateString: "2017-07-06T12:35:37.513"},
        {_id: 4, dateString: "1960-07-10T12:10:37.448"},
    ]));

    assert.eq(
        [
          {"_id": 0, "date": ISODate("2017-07-06T12:35:37Z")},
          {"_id": 1, "date": ISODate("2017-07-06T12:35:37.513Z")},
          {"_id": 2, "date": ISODate("2017-07-06T12:35:37Z")},
          {"_id": 3, "date": ISODate("2017-07-06T12:35:37.513Z")},
          {"_id": 4, "date": ISODate("1960-07-10T12:10:37.448Z")},
        ],
        coll.aggregate([
                {
                  $project: {date: {'$dateFromString': {dateString: "$dateString"}}},
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    assert.eq(
        [
          {"_id": 0, "date": new Date(1499344537000)},
          {"_id": 1, "date": new Date(1499344537513)},
          {"_id": 2, "date": new Date(1499344537000)},
          {"_id": 3, "date": new Date(1499344537513)},
          {"_id": 4, "date": new Date(-299072962552)},
        ],
        coll.aggregate([
                {
                  $project: {date: {'$dateFromString': {dateString: "$dateString"}}},
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* BI format tests. */

    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));

    let pipelines = [
        {
          expect: "2017-01-01T00:00:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-01-01 00:00:00"}}}}
        },
        {
          expect: "2017-07-01T00:00:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-01 00:00:00"}}}}
        },
        {
          expect: "2017-07-06T00:00:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-06"}}}}
        },
        {
          expect: "2017-07-06T00:00:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-06 00:00:00"}}}}
        },
        {
          expect: "2017-07-06T11:00:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-06 11:00:00"}}}}
        },
        {
          expect: "2017-07-06T11:36:00Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-06 11:36:00"}}}}
        },
        {
          expect: "2017-07-06T11:36:54Z",
          pipeline: {$project: {date: {'$dateFromString': {"dateString": "2017-07-06 11:36:54"}}}}
        },
    ];
    pipelines.forEach(function(pipeline) {
        assert.eq([{_id: 0, date: ISODate(pipeline.expect)}],
                  coll.aggregate(pipeline.pipeline).toArray(),
                  tojson(pipeline));
    });

    /* --------------------------------------------------------------------------------------- */
    /* BI format tests from data. */

    coll.drop();
    assert.writeOK(coll.insert([
        {_id: 0, dateString: "2017-01-01 00:00:00"},
        {_id: 1, dateString: "2017-07-01 00:00:00"},
        {_id: 2, dateString: "2017-07-06"},
        {_id: 3, dateString: "2017-07-06 00:00:00"},
        {_id: 4, dateString: "2017-07-06 11:00:00"},
        {_id: 5, dateString: "2017-07-06 11:36:00"},
        {_id: 6, dateString: "2017-07-06 11:36:54"},
    ]));

    assert.eq(
        [
          {"_id": 0, "date": ISODate("2017-01-01T00:00:00Z")},
          {"_id": 1, "date": ISODate("2017-07-01T00:00:00Z")},
          {"_id": 2, "date": ISODate("2017-07-06T00:00:00Z")},
          {"_id": 3, "date": ISODate("2017-07-06T00:00:00Z")},
          {"_id": 4, "date": ISODate("2017-07-06T11:00:00Z")},
          {"_id": 5, "date": ISODate("2017-07-06T11:36:00Z")},
          {"_id": 6, "date": ISODate("2017-07-06T11:36:54Z")}
        ],
        coll.aggregate([
                {
                  $project: {date: {'$dateFromString': {dateString: "$dateString"}}},
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Wacky format tests from data. */

    coll.drop();
    assert.writeOK(coll.insert([
        {_id: 0, dateString: "July 4th, 2017"},
        {_id: 1, dateString: "July 4th, 2017 12:39:30 BST"},
        {_id: 2, dateString: "July 4th, 2017 11am"},
        {_id: 3, dateString: "July 4th, 2017 12pm"},
        {_id: 4, dateString: "7/4/17"},
        {_id: 5, dateString: "04-07-2017"},
        {_id: 6, dateString: "2017-Jul-04 noon"},
        {_id: 7, dateString: "2017-07-04 12:48:07 GMT+0545"},
        {_id: 8, dateString: "2017-07-04 12:48:07 GMT-0200"},
    ]));

    assert.eq(
        [
          {"_id": 0, "date": ISODate("2017-07-04T00:00:00Z")},
          {"_id": 1, "date": ISODate("2017-07-04T11:39:30Z")},
          {"_id": 2, "date": ISODate("2017-07-04T11:00:00Z")},
          {"_id": 3, "date": ISODate("2017-07-04T12:00:00Z")},
          {"_id": 4, "date": ISODate("2017-07-04T00:00:00Z")},
          {"_id": 5, "date": ISODate("2017-07-04T00:00:00Z")},
          {"_id": 6, "date": ISODate("2017-07-04T12:00:00Z")},
          {"_id": 7, "date": ISODate("2017-07-04T07:03:07Z")},
          {"_id": 8, "date": ISODate("2017-07-04T14:48:07Z")},
        ],
        coll.aggregate([
                {
                  $project: {date: {'$dateFromString': {dateString: "$dateString"}}},
                },
                {$sort: {_id: 1}}
            ])
            .toArray());

    /* --------------------------------------------------------------------------------------- */
    /* Testing whether it throws the right assert for missing elements of a date/time string. */

    coll.drop();

    assert.writeOK(coll.insert([
        {_id: 0},
    ]));

    pipelines = [
        [{'$project': {date: {'$dateFromString': {dateString: "July 4th"}}}}],
        [{'$project': {date: {'$dateFromString': {dateString: "2017, 12:50:53"}}}}],
    ];

    pipelines.forEach(function(pipeline) {
        assertErrorCode(coll, pipeline, 40545, tojson(pipeline));
    });

    /* --------------------------------------------------------------------------------------- */
    /* NULL returns. */

    coll.drop();
    assert.writeOK(coll.insert([
        {_id: 0, date: new ISODate("2017-06-19T15:13:25.713Z")},
        {_id: 1, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: null},
        {_id: 2, date: new ISODate("2017-06-19T15:13:25.713Z"), tz: undefined},
    ]));

    pipelines = [
        [{$project: {date: {'$dateFromString': {"dateString": "$tz"}}}}, {$sort: {_id: 1}}],
    ];
    pipelines.forEach(function(pipeline) {
        assert.eq([{_id: 0, date: null}, {_id: 1, date: null}, {_id: 2, date: null}],
                  coll.aggregate(pipeline).toArray(),
                  tojson(pipeline));
    });

    /* --------------------------------------------------------------------------------------- */
    /* Parse errors. */

    let pipeline = {$project: {date: {'$dateFromString': "no-object"}}};
    assertErrorCode(coll, pipeline, 40540);

    pipeline = {$project: {date: {'$dateFromString': {"unknown": "$tz"}}}};
    assertErrorCode(coll, pipeline, 40541);

    pipeline = {$project: {date: {'$dateFromString': {"dateString": 5}}}};
    assertErrorCode(coll, pipeline, 40543);

})();
