/**
 * Test that the query stats store can be cleared when the cache size is reset to 0.
 * @tags: [requires_fcv_72]
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";

// Turn on the collecting of queryStats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsCacheSize: "10MB"},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

let query = {};
for (var j = 0; j < 10; ++j) {
    query["foo.field.xyz." + j] = 1;
    query["bar.field.xyz." + j] = 2;
    query["baz.field.xyz." + j] = 3;
    coll.aggregate([{$match: query}]).itcount();
}

// Confirm number of entries in the store and that none have been evicted.
let res = getQueryStats(conn, {collName: coll.getName()});
assert.eq(res.length, 10, res);
{
    const metrics = testDB.serverStatus().metrics.queryStats;
    assert.eq(metrics.numEvicted, 0, metrics);
    assert.gt(metrics.queryStatsStoreSizeEstimateBytes, 0, metrics);
    // This test directly caused 11 queries to be inserted (10 queries above + $queryStats), but
    // there may be background queries that also got collected, so we'll make this assertion
    // lenient.
    assert.gte(metrics.numEntries, 11, metrics);
}

// Command to clear the cache.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));

// 10 regular queries plus the $queryStats query, means 11 entries evicted when the cache is
// cleared.
{
    const metrics = testDB.serverStatus().metrics.queryStats;
    assert.gte(metrics.numEvicted, 11, metrics);
    assert.eq(metrics.queryStatsStoreSizeEstimateBytes, 0, metrics);
    assert.eq(metrics.numEntries, 0, metrics);
}

// Calling $queryStats should fail when the query stats store size is 0 bytes.
assert.throwsWithCode(() => testDB.getSiblingDB("admin").aggregate([{$queryStats: {}}]), 6579000);
MongoRunner.stopMongod(conn);
