/*
 * Intergration test for read preference with hedging mode. The more comprehensive
 * unit test can be found in dbtests/read_preference_test.cpp.
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

assert.commandWorked(
    testDB.runCommand({query: {find: collName}, $readPreference: {mode: "nearest", hedge: {}}}));

assert.commandWorked(testDB.runCommand({
    query: {distinct: collName, key: "x"},
    $readPreference: {mode: "primaryPreferred", hedge: {enabled: true, delay: false}}
}));

assert.commandFailedWithCode(
    testDB.runCommand({query: {count: collName}, $readPreference: {mode: "primary", hedge: {}}}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(testDB.runCommand({
    $query: {
        explain: {aggregate: collName, pipeline: [], cursor: {}},
    },
    $readPreference: {mode: "secondary", hedge: {enabled: false, delay: true}}
}),
                             ErrorCodes.InvalidOptions);

st.stop();
})();
