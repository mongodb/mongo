/**
 * Test that applicationName and namespace appear in queryStats for the find command.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats, getQueryStatsFindCmd} from "jstests/libs/query/query_stats_utils.js";

const kApplicationName = "MongoDB Shell";

// Turn on the collecting of queryStats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

const conn = MongoRunner.runMongod(options);
conn.setLogLevel(3, "query");
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

coll.insert({v: 1});
coll.insert({v: 2});
coll.insert({v: 3});

coll.find({v: 1}).toArray();

let queryStats = getQueryStats(conn);
assert.eq(1, queryStats.length, queryStats);
assert.eq(kApplicationName, queryStats[0].key.client.application.name, queryStats);

queryStats = getQueryStatsFindCmd(conn, {transformIdentifiers: true});
assert.eq(1, queryStats.length, queryStats);
assert.eq(kApplicationName, queryStats[0].key.client.application.name, queryStats);

MongoRunner.stopMongod(conn);
