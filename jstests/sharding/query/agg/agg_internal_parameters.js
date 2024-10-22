/**
 * Tests that mongoS rejects 'aggregate' commands which explicitly set any of the
 * parameters that mongoS uses internally when communicating with the shards.
 * @tags: [
 *   requires_fcv_81,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));

// Test that command succeeds when no internal options have been specified.
assert.commandWorked(
    mongosDB.runCommand({aggregate: mongosColl.getName(), pipeline: [], cursor: {}}));

// Test that the command fails if we have 'needsMerge: false' without 'fromRouter'.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, needsMerge: false}),
    ErrorCodes.FailedToParse);

// Test that the command fails if we have 'needsMerge: true' without 'fromRouter'.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, needsMerge: true}),
    ErrorCodes.FailedToParse);

// Test that the command fails if we have 'isClusterQueryWithoutShardKeyCmd: true' without
// 'fromRouter'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    $_isClusterQueryWithoutShardKeyCmd: true
}),
                             ErrorCodes.InvalidOptions);

// Test that the command fails if we have 'isClusterQueryWithoutShardKeyCmd: true' with
// 'fromRouter: false'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    fromRouter: false,
    $_isClusterQueryWithoutShardKeyCmd: true
}),
                             ErrorCodes.InvalidOptions);

// Test that 'fromRouter: true' cannot be specified in a command sent to mongoS.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, fromRouter: true}),
    51089);

// Test that 'fromRouter: false' can be specified in a command sent to mongoS.
assert.commandWorked(mongosDB.runCommand(
    {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, fromRouter: false}));

// Test that the command fails if we have 'needsMerge: true' with 'fromRouter: false'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromRouter: false
}),
                             51089);

// Test that the command fails if we have 'needsMerge: true' with 'fromRouter: true'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromRouter: true
}),
                             51089);

// Test that 'needsMerge: false' can be specified in a command sent to mongoS along with
// 'fromRouter: false'.
assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: false,
    fromRouter: false
}));

// Test that the 'exchange' parameter cannot be specified in a command sent to mongoS.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    exchange: {policy: 'roundrobin', consumers: NumberInt(2)}
}),
                             51028);

// Test that the command fails when all internal parameters have been specified.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromRouter: true,
    exchange: {policy: 'roundrobin', consumers: NumberInt(2)}
}),
                             51028);

// Test that the command fails when all internal parameters but exchange have been specified.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromRouter: true
}),
                             51089);

st.stop();
