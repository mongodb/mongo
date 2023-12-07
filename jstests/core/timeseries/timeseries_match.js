/**
 * Tests $match path semantics for time series collections.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    let coll = db.timeseries_match;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';
    const metaFieldName = 'measurement';

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    insert(coll, {
        _id: 0,
        [timeFieldName]: new Date(datePrefix + 100),
        [metaFieldName]: "cpu",
        topLevelScalar: 123,
        topLevelArray: [1, 2, 3, 4],
        arrOfObj: [{x: 1}, {x: 2}, {x: 3}, {x: 4}],
    });
    insert(coll, {
        _id: 1,
        [timeFieldName]: new Date(datePrefix + 200),
        [metaFieldName]: "cpu",
        topLevelScalar: 456,
        topLevelArray: [101, 102, 103, 104],
        arrOfObj: [{x: 101}, {x: 102}, {x: 103}, {x: 104}],
    });
    insert(coll, {
        _id: 2,
        [timeFieldName]: new Date(datePrefix + 300),
        [metaFieldName]: "cpu",

        // All fields missing.
    });

    //
    // These queries are written in an odd way so that SBE can be used.
    //

    /* $match tests */

    function testMatch(filter, ids) {
        let res = coll.aggregate([{$match: filter}, {$project: {_id: 1}}]).toArray();

        assert.eq(res, ids);
    }

    testMatch({"topLevelScalar": {$gt: 123}}, [{_id: 1}]);
    testMatch({"topLevelScalar": {$gte: 123}}, [{_id: 0}, {_id: 1}]);
    testMatch({"topLevelScalar": {$lt: 456}}, [{_id: 0}]);
    testMatch({"topLevelScalar": {$lte: 456}}, [{_id: 0}, {_id: 1}]);
    testMatch({"topLevelScalar": {$eq: 456}}, [{_id: 1}]);
    testMatch({"topLevelScalar": {$ne: 456}}, [{_id: 0}, {_id: 2}]);

    testMatch({"topLevelArray": {$gt: 4}}, [{_id: 1}]);
    testMatch({"topLevelArray": {$gte: 4}}, [{_id: 0}, {_id: 1}]);
    testMatch({"topLevelArray": {$lt: 101}}, [{_id: 0}]);
    testMatch({"topLevelArray": {$lte: 101}}, [{_id: 0}, {_id: 1}]);
    testMatch({"topLevelArray": {$eq: 102}}, [{_id: 1}]);
    testMatch({"topLevelArray": {$ne: 102}}, [{_id: 0}, {_id: 2}]);

    testMatch({"arrOfObj.x": {$gt: 4}}, [{_id: 1}]);
    testMatch({"arrOfObj.x": {$gte: 4}}, [{_id: 0}, {_id: 1}]);
    testMatch({"arrOfObj.x": {$lt: 101}}, [{_id: 0}]);
    testMatch({"arrOfObj.x": {$lte: 101}}, [{_id: 0}, {_id: 1}]);
    testMatch({"arrOfObj.x": {$eq: 102}}, [{_id: 1}]);
    testMatch({"arrOfObj.x": {$ne: 102}}, [{_id: 0}, {_id: 2}]);

    testMatch({"time": {$gt: new Date(datePrefix + 100)}}, [{_id: 1}, {_id: 2}]);
    testMatch({"time": {$gte: new Date(datePrefix + 100)}}, [{_id: 0}, {_id: 1}, {_id: 2}]);
    testMatch({"time": {$lt: new Date(datePrefix + 200)}}, [{_id: 0}]);
    testMatch({"time": {$lte: new Date(datePrefix + 200)}}, [{_id: 0}, {_id: 1}]);
    testMatch({"time": {$eq: new Date(datePrefix + 300)}}, [{_id: 2}]);
    testMatch({"time": {$ne: new Date(datePrefix + 200)}}, [{_id: 0}, {_id: 2}]);

    testMatch({"time": {$gt: new Date(datePrefix + 100), $lt: new Date(datePrefix + 300)}},
              [{_id: 1}]);

    testMatch({"time": {"obj": new Date(datePrefix + 100)}}, []);

    // Special test case which can result in an empty event filter being compiled (SERVER-84001).
    {
        const pipe = [
            {$match: {"topLevelScalarField": {$not: {$in: []}}}},
            {$match: {"measurement": "cpu"}},
            {$project: {_id: 1}}
        ];
        const res = coll.aggregate(pipe).toArray()
        assert.eq(res.length, 3);
    }
});
