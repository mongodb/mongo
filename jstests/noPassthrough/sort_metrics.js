// Test metrics.query.sort.* ServerStatus counters
(function() {
'use strict';

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
assert.commandWorked(db.dropDatabase());
const coll = db.spill_to_disk;

const memoryLimitMB = 100;
const bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','
for (let i = 0; i < memoryLimitMB + 1; i++)
    assert.commandWorked(coll.insert({_id: i, bigStr: i + bigStr, random: Math.random()}));
assert.gt(coll.stats().size, memoryLimitMB * 1024 * 1024);

const metricsBefore = db.serverStatus().metrics.query.sort;

const pipeline = [{$sort: {random: 1}}];
assert.eq(coll.aggregate(pipeline).itcount(), coll.count());

const metricsAfter = db.serverStatus().metrics.query.sort;
assert.gt(metricsAfter.spillToDisk,
          metricsBefore.spillToDisk,
          "Expect metric query.sort.spillToDisk to increment after pipeline " + tojson(pipeline));
assert.gt(
    metricsAfter.totalKeysSorted,
    metricsBefore.totalKeysSorted,
    "Expect metric query.sort.totalKeysSorted to increment after pipeline " + tojson(pipeline));
assert.gt(
    metricsAfter.totalKeysSorted,
    metricsBefore.totalKeysSorted,
    "Expect metric query.sort.totalKeysSorted to increment after pipeline " + tojson(pipeline));

MongoRunner.stopMongod(conn);
})();
