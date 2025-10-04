/**
 * Tests what values are accepted for the maxAcceptableLogicalClockDriftSecs startup parameter, and
 * that servers in a sharded clusters reject cluster times more than
 * maxAcceptableLogicalClockDriftSecs ahead of their wall clocks.
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// maxAcceptableLogicalClockDriftSecs cannot be negative, zero, or a non-number.
assert.throws(
    () => MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: -1}}),
    [],
    "expected server to reject negative maxAcceptableLogicalClockDriftSecs",
);

assert.throws(
    () => MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: 0}}),
    [],
    "expected server to reject zero maxAcceptableLogicalClockDriftSecs",
);

assert.throws(
    () => MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: "value"}}),
    [],
    "expected server to reject non-numeric maxAcceptableLogicalClockDriftSecs",
);

// Any positive number is valid.
let conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: 1}});
assert.neq(null, conn, "failed to start mongod with valid maxAcceptableLogicalClockDriftSecs");
MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({setParameter: {maxAcceptableLogicalClockDriftSecs: 60 * 60 * 24 * 365 * 10}}); // 10 years.
assert.neq(null, conn, "failed to start mongod with valid maxAcceptableLogicalClockDriftSecs");
MongoRunner.stopMongod(conn);

// Verify maxAcceptableLogicalClockDriftSecs works as expected in a sharded cluster.
const maxDriftValue = 100;
const st = new ShardingTest({
    shards: 1,
    rsOptions: {setParameter: {maxAcceptableLogicalClockDriftSecs: maxDriftValue}},
    mongosOptions: {setParameter: {maxAcceptableLogicalClockDriftSecs: maxDriftValue}},
});
let testDB = st.s.getDB("test");

// Contact cluster to get initial cluster time.
let res = assert.commandWorked(testDB.runCommand({hello: 1}));
let lt = res.$clusterTime;

// Try to advance cluster time by more than the max acceptable drift, which should fail the rate
// limiter.
let tooFarTime = Object.assign({}, lt, {clusterTime: new Timestamp(lt.clusterTime.getTime() + maxDriftValue * 2, 0)});
assert.commandFailedWithCode(
    testDB.runCommand({hello: 1, $clusterTime: tooFarTime}),
    ErrorCodes.ClusterTimeFailsRateLimiter,
    "expected command to not pass the rate limiter",
);

st.stop();
