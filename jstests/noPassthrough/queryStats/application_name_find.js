/**
 * Test that applicationName and namespace appear in queryStats for the find command.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/query_stats_utils.js");
(function() {
"use strict";

const kApplicationName = "MongoDB Shell";
const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";
const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

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
}());
