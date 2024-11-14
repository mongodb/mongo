// Checks that global histogram counters for collections are updated as we expect.
// @tags: [
//   requires_replication,
//   featureFlagRouterPort,
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

// Run tests against embedded mongos.
const st = new ShardingTest({config: 1, shards: 1, embeddedRouter: true});
let testDB = st.s.getDB("test");
let testColl = testDB[latencyName + "coll"];
testColl.drop();
runLatencyComparisonTest(opLatencies, st, testDB, testColl);
runTests(getLatencyHistogramStats, testDB, testColl, true);

testColl = testDB[workingTimeName + "coll"];
testColl.drop();
runLatencyComparisonTest(opWorkingTime, st, testDB, testColl);
runTests(getWorkingTimeHistogramStats, testDB, testColl, true);

st.stop();
