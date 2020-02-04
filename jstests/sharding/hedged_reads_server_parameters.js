/*
 * Intergration test for the server parameters for hedged reads. The more comprehensive
 * unit test can be found in executor/remote_command_request_test.cpp.
 *
 * TODO (SERVER-45432): test that hedging is performed as expected.
 *
 * @tags: [requires_fcv_44]
 */
(function() {

const st = new ShardingTest({shards: 2});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand(
    {setParameter: 1, maxTimeMSThresholdForHedging: 1500, hedgingDelayPercentage: 50}));

// With maxTimeMS < maxTimeMSThresholdForHedging, expect hedging.
assert.commandWorked(st.s.getDB(dbName).runCommand(
    {query: {find: collName, maxTimeMS: 1000}, $readPreference: {mode: "nearest", hedge: {}}}));

// With maxTimeMS > maxTimeMSThresholdForHedging, expect no hedging.
assert.commandWorked(st.s.getDB(dbName).runCommand(
    {query: {find: collName, maxTimeMS: 1600}, $readPreference: {mode: "nearest", hedge: {}}}));

// Without maxTimeMS, expect hedging.
assert.commandWorked(st.s.adminCommand({setParameter: 1, defaultHedgingDelayMS: 800}));

assert.commandWorked(st.s.getDB(dbName).runCommand(
    {query: {count: collName}, $readPreference: {mode: "secondaryPreferred", hedge: {}}}));

// readHedgingMode "off", expect no hedging.
st.s.adminCommand({setParameter: 1, readHedgingMode: "off"});

assert.commandWorked(st.s.getDB(dbName).runCommand({
    query: {distinct: collName, key: "x"},
    $readPreference: {mode: "primaryPreferred", hedge: {}}
}));

// Test server parameter validation.
assert.commandFailedWithCode(st.s.adminCommand({setParameter: 1, maxTimeMSThresholdForHedging: -1}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(st.s.adminCommand({setParameter: 1, hedgingDelayPercentage: -1}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(st.s.adminCommand({setParameter: 1, hedgingDelayPercentage: 101}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(st.s.adminCommand({setParameter: 1, defaultHedgingDelayMS: -1}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(st.s.adminCommand({setParameter: 1, readHedgingMode: "invalidMode"}),
                             ErrorCodes.BadValue);

st.stop();
})();
