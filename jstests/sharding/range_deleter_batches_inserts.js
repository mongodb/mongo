/**
 *  @tags: [requires_fcv_44]
 */
(function() {

'use strict';

// Skip checking orphans because it takes a long time when there are so many chunks. Also we don't
// actually insert any data so there can't be orphans.
TestData.skipCheckOrphans = true;

const dbName = "db1";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({
    shards: 2,
    config: 1,
    other: {
        // Set the migration verbosity to 0 to suppress log lines from processing range deletion
        // tasks.
        shardOptions: {setParameter: {logComponentVerbosity: tojson({sharding: {migration: 0}})}},
        // Set the transaction verbosity to 0 to suppress log lines from upgrading the config.chunks
        // schema on setFCV to 4.4.
        configOptions: {setParameter: {logComponentVerbosity: tojson({transaction: 0})}},
    }
});

// Set the slow query sample rate on the "config" database to 0% to suppress log lines from
// upgrading the config.chunks schema on setFCV to 4.4.
assert.commandWorked(
    st.configRS.getPrimary().getDB("config").setProfilingLevel(1, {sampleRate: 0}));

jsTest.log("Set FCV to 4.2 since we want to test upgrading the FCV to 4.4");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

jsTest.log("Create a database.");
// enableSharding creates the database.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

jsTest.log("Shard a collection with a huge number of initial chunks");
const NUM_CHUNKS = 100000;
assert.commandWorked(
    st.s.adminCommand({shardCollection: ns, key: {x: "hashed"}, numInitialChunks: NUM_CHUNKS}));
assert.gt(st.s.getDB("config").chunks.count(), NUM_CHUNKS - 1);

jsTest.log("Set FCV to 4.4");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

st.stop();
})();
