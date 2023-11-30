/**
 * Test that queryStats works properly for a find command that uses regex.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats} from "jstests/libs/query_stats_utils.js";

// Turn on the collecting of queryStats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 100;
for (let i = 0; i < numDocs / 2; ++i) {
    bulk.insert({foo: "ABCDE"});
    bulk.insert({foo: "CDEFG"});
}
assert.commandWorked(bulk.execute());

{
    coll.find({foo: {$regex: "/^ABC/i"}}).itcount();
    let queryStats = getQueryStats(testDB);
    assert.eq(1, queryStats.length, queryStats);
    assert.eq({"foo": {"$regex": "?string"}}, queryStats[0].key.queryShape.filter);
}

MongoRunner.stopMongod(conn);
