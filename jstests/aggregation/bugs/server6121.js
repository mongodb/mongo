/*
 * SERVER-6121: aggregation framework converts Timestamp to long long in result set
 *
 * This test validates the SERVER-6121 ticket. Add support for timestamps to Aggregation and
 * ensure they can do everything dates can. Previously timestamps were awkwardly used as dates
 * and long longs.
 */

/*
 * 1) Clear and create testing db
 * 2) Run an aggregation with all date expressions on a timestamp and a date
 * 3) Run an aggregation that will show timestamp and date can not be compared
 * 4) Run an aggregation comparing two timestamps to show inc matters
 */

// Clear db
db.s6121.drop();
// Populate db
db.s6121.save({date:new Timestamp(1341337661000, 1)});
db.s6121.save({date:new Date(1341337661000)});
// Aggregate checking various combinations of the constant and the field
var s6121 = db.runCommand(
    {aggregate: "s6121", pipeline: [
        {$project: {
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
        }}
]});
// Assert the two entries are equal
assert.eq(s6121.result[0], s6121.result[1], 's6121 failed');


// Clear db for timestamp to date compare test
db.s6121.drop();
db.s6121.save({time:new Timestamp(1341337661000, 1), date:new Date(1341337661000)});
var s6121 = db.runCommand(
    {aggregate: "s6121", pipeline: [
        {$project: {
            _id: 0,
            dates_arent_times: {$eq: ['$time', '$date']}
        }}
]});
// Assert we get the error we want
assert.eq(s6121.ok, 0, 's6121 failed confirming that date and timestamp cant be compared');
assert.eq(s6121.code, 15994, 's6121 failed confirming that date and timestamp cant be compared');


// Clear db for timestamp comparison tests
db.s6121.drop();
db.s6121.save({time:new Timestamp(1341337661000, 1), time2:new Timestamp(1341337661000, 2)});
var s6121 = db.runCommand(
    {aggregate: "s6121", pipeline: [
        {$project: {
            _id: 0,
            cmp: {$cmp: ['$time', '$time2']},
            eq: {$eq: ['$time', '$time2']},
            gt: {$gt: ['$time', '$time2']},
            gte: {$gte: ['$time', '$time2']},
            lt: {$lt: ['$time', '$time2']},
            lte: {$lte: ['$time', '$time2']},
            ne: {$ne: ['$time', '$time2']}
        }}
]});
var s6121result = [{
    cmp: -1,
    eq: false,
    gt: false,
    gte: false,
    lt: true,
    lte: true,
    ne: true
}];
// Assert the results are as expected
assert.eq(s6121.result, s6121result, 's6121 failed comparing two timestamps');
