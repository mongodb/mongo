/**
 * Tests that mongoS rejects 'aggregate' commands which explicitly set any of the
 * parameters that mongoS uses internally when communicating with the shards.
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 2, rs: {nodes: 1, enableMajorityReadConcern: ''}});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Test that command succeeds when no internal options have been specified.
assert.commandWorked(
    mongosDB.runCommand({aggregate: mongosColl.getName(), pipeline: [], cursor: {}}));

// Test that the command fails if we have 'needsMerge: false' without 'fromMongos'.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, needsMerge: false}),
    ErrorCodes.FailedToParse);

// Test that the command fails if we have 'needsMerge: true' without 'fromMongos'.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, needsMerge: true}),
    ErrorCodes.FailedToParse);

// Test that 'fromMongos: true' cannot be specified in a command sent to mongoS.
assert.commandFailedWithCode(
    mongosDB.runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, fromMongos: true}),
    51089);

// Test that 'fromMongos: false' can be specified in a command sent to mongoS.
assert.commandWorked(mongosDB.runCommand(
    {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, fromMongos: false}));

// Test that the command fails if we have 'needsMerge: true' with 'fromMongos: false'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromMongos: false
}),
                             51089);

// Test that the command fails if we have 'needsMerge: true' with 'fromMongos: true'.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromMongos: true
}),
                             51089);

// Test that 'needsMerge: false' can be specified in a command sent to mongoS along with
// 'fromMongos: false'.
assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: false,
    fromMongos: false
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
    fromMongos: true,
    exchange: {policy: 'roundrobin', consumers: NumberInt(2)}
}),
                             51028);

// Test that the command fails when all internal parameters but exchange have been specified.
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [],
    cursor: {},
    needsMerge: true,
    fromMongos: true
}),
                             51089);

st.stop();
})();
