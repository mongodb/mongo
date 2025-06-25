/**
 * Tests that inserting data into a time-series collection records latencyStats in $collStats.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_getmore,
 * ]
 */
const conn = MongoRunner.runMongod();

const testDB = conn.getDB('test');
const coll = testDB[jsTestName()];

coll.drop();

const timeFieldName = 'time';
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const getLatencyStats = () => {
    const stats = coll.aggregate([{$collStats: {latencyStats: {}}}]).next();
    assert(stats.hasOwnProperty("latencyStats"), tojson(stats));
    assert(stats.latencyStats.hasOwnProperty("writes"), tojson(stats));
    return stats.latencyStats.writes;
};

const stats1 = getLatencyStats();
assert.eq(stats1.ops, 0, tojson(stats1));
assert.eq(stats1.latency, 0, tojson(stats1));

assert.commandWorked(coll.insert({[timeFieldName]: new Date(), x: 1}));

const stats2 = getLatencyStats();
assert.eq(stats2.ops, 1, tojson(stats2));
assert.gt(stats2.latency, stats1.latency, tojson(stats2));

const reps = 10;
for (let i = 0; i < reps; ++i) {
    assert.commandWorked(coll.insert({[timeFieldName]: new Date(), x: 1}));
}

const stats3 = getLatencyStats();
assert.eq(stats3.ops, 1 + reps, tojson(stats3));
assert.gt(stats3.latency, stats2.latency, tojson(stats3));

MongoRunner.stopMongod(conn);
