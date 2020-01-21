/*
 * Intergration test for the server parameters for hedged reads. The more comprehensive
 * unit test can be found in executor/remote_command_request_test.cpp.
 *
 * TODO (SERVER-45432): test that hedging is performed as expected.
 *
 * @tags: [requires_fcv_44]
 */
(function() {

const st = new ShardingTest({
    shards: 2,
    mongosOptions: {setParameter: {maxTimeMSThresholdForHedging: 1500, hedgingDelayPercentage: 50}}
});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

// With maxTimeMS.
assert.commandWorked(st.s.getDB(dbName).runCommand(
    {query: {find: collName, maxTimeMS: 1000}, $readPreference: {mode: "nearest", hedge: {}}}));

// Without maxTimeMS.
st.restartMongos(0, {
    restart: true,
    setParameter: {defaultHedgingDelayMS: 800},
});

assert.commandWorked(st.s.getDB(dbName).runCommand(
    {query: {count: collName}, $readPreference: {mode: "secondaryPreferred", hedge: {}}}));

st.stop();
})();
