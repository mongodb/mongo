// Checks that global histogram counters for collections are updated as we expect.
// @tags: [
//   requires_replication,
// ]

import {
    getLatencyHistogramStats,
    getWorkingTimeHistogramStats,
    opLatencies,
    opWorkingTime,
    runLatencyComparisonTest,
    runTests
} from "jstests/libs/global_latency_histogram.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const latencyName = "opLatencyHistogramTest";
const workingTimeName = "opWorkingTimeHistogramTest";

// Run tests against mongod.
const mongod = MongoRunner.runMongod();
let testDB = mongod.getDB(jsTestName());
let testColl = testDB[latencyName + "coll"];
testColl.drop();
runTests(getLatencyHistogramStats, testDB, testColl, false);

testColl = testDB[workingTimeName + "coll"];
testColl.drop();
runTests(getWorkingTimeHistogramStats, testDB, testColl, false);

MongoRunner.stopMongod(mongod);

// Run tests against mongos.
const st = new ShardingTest({config: 1, shards: 1});
testDB = st.s.getDB(jsTestName());
testColl = testDB[latencyName + "coll"];
testColl.drop();
runLatencyComparisonTest(opLatencies, st, testDB, testColl);
runTests(getLatencyHistogramStats, testDB, testColl, true);

testColl = testDB[workingTimeName + "coll"];
testColl.drop();
runLatencyComparisonTest(opWorkingTime, st, testDB, testColl);
runTests(getWorkingTimeHistogramStats, testDB, testColl, true);

st.stop();
