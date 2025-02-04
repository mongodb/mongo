/**
 * SERVER-6121: aggregation framework converts Timestamp to long long in result set
 *
 * This test validates the SERVER-6121 ticket. Add support for timestamps to Aggregation and
 * ensure they can do everything dates can. Previously timestamps were awkwardly used as dates
 * and long longs.
 */

/*
 * 1) Clear and create testing db
 * 2) Run an aggregation with all date expressions on a timestamp and a date
 * 3) Run an aggregation that will show timestamp and date can be compared
 * 4) Run an aggregation comparing two timestamps to show inc matters
 */

let coll = db[jsTestName()];
// Clear db
coll.drop();
// Populate db
assert.commandWorked(coll.insertOne({date: new Timestamp(1341337661, 1)}));
assert.commandWorked(coll.insertOne({date: new Date(1341337661000)}));
// Aggregate checking various combinations of the constant and the field
let agg_timestamp = coll.aggregate({
                            $project: {
                                _id: 0,
                                dayOfMonth: {$dayOfMonth: '$date'},
                                dayOfWeek: {$dayOfWeek: '$date'},
                                dayOfYear: {$dayOfYear: '$date'},
                                hour: {$hour: '$date'},
                                minute: {$minute: '$date'},
                                month: {$month: '$date'},
                                second: {$second: '$date'},
                                week: {$week: '$date'},
                                year: {$year: '$date'}
                            }
                        })
                        .toArray();
// Assert the two entries are equal
assert.eq(agg_timestamp[0], agg_timestamp[1], 'agg_timestamp failed');

// Clear db for timestamp to date compare test
// For historical reasons the compare the same if they are the same 64-bit representation.
// That means that the Timestamp has an "inc" that is the same as the Date has millis.
assert(coll.drop());
assert.commandWorked(coll.insertOne({time: new Timestamp(0, 1234), date: new Date(1234)}));
assert.commandWorked(coll.insertOne({time: new Timestamp(1, 1234), date: new Date(1234)}));
printjson(coll.find().toArray());
agg_timestamp = coll.aggregate({
    $project: {
        _id: 0,
        // comparison is different code path based on order (same as in bson)
        ts_date: {$eq: ['$time', '$date']},
        date_ts: {$eq: ['$date', '$time']}
    }
});
assert.eq(agg_timestamp.toArray(),
          [{ts_date: false, date_ts: false}, {ts_date: false, date_ts: false}]);

// Clear db for timestamp comparison tests
assert(coll.drop());
assert.commandWorked(
    coll.insertOne({time: new Timestamp(1341337661, 1), time2: new Timestamp(1341337661, 2)}));
agg_timestamp = coll.aggregate({
    $project: {
        _id: 0,
        cmp: {$cmp: ['$time', '$time2']},
        eq: {$eq: ['$time', '$time2']},
        gt: {$gt: ['$time', '$time2']},
        gte: {$gte: ['$time', '$time2']},
        lt: {$lt: ['$time', '$time2']},
        lte: {$lte: ['$time', '$time2']},
        ne: {$ne: ['$time', '$time2']}
    }
});
var agg_timestampresult =
    [{cmp: -1, eq: false, gt: false, gte: false, lt: true, lte: true, ne: true}];
// Assert the results are as expected
assert.eq(
    agg_timestamp.toArray(), agg_timestampresult, 'agg_timestamp failed comparing two timestamps');
