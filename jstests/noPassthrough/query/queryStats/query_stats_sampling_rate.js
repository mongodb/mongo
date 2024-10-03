/**
 * Test that calls to read from query stats store fail when sampling rate is not greater than 0 even
 * if feature flag is on.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";

let options = {
    setParameter: {internalQueryStatsRateLimit: 0},
};

const conn = MongoRunner.runMongod(options);
const testdb = conn.getDB('test');
var coll = testdb[jsTestName()];
coll.drop();
for (var i = 0; i < 20; i++) {
    coll.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
}

coll.find({foo: 1}).batchSize(2).toArray();

// Reading query stats store with a sampling rate of 0 should return 0 documents.
let stats = getQueryStats(testdb);
assert.eq(stats.length, 0);

// Reading query stats store should work now with a sampling rate of greater than 0.
assert.commandWorked(testdb.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}));
coll.find({foo: 1}).batchSize(2).toArray();
stats = getQueryStats(testdb);
assert.eq(stats.length, 1);

MongoRunner.stopMongod(conn);
