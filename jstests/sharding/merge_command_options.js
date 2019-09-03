// Tests that aggregations with a $merge stage respect the options set on the command.
(function() {
'use strict';

load("jstests/libs/profiler.js");  // For profilerHasNumMatchingEntriesOrThrow.

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const mongosDB = st.s0.getDB("test");
const source = mongosDB.getCollection("source");
const target = mongosDB.getCollection("target");
const primaryDB = st.rs0.getPrimary().getDB("test");
const nonPrimaryDB = st.rs1.getPrimary().getDB("test");
const maxTimeMS = 5 * 60 * 1000;

// Enable profiling on the test DB.
assert.commandWorked(primaryDB.setProfilingLevel(2));
assert.commandWorked(nonPrimaryDB.setProfilingLevel(2));

// Enable sharding on the test DB and ensure that shard0 is the primary.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Shard the target collection, and set the unique flag to ensure that there's a unique
// index on the shard key.
const shardKey = {
    sk: 1
};
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: target.getFullName(), key: shardKey, unique: true}));
assert.commandWorked(mongosDB.adminCommand({split: target.getFullName(), middle: {sk: 1}}));
assert.commandWorked(
    mongosDB.adminCommand({moveChunk: target.getFullName(), find: {sk: 1}, to: st.rs1.getURL()}));

assert.commandWorked(source.insert({sk: "dummy"}));

// The shardCollection command will send a listIndexes on the target collection.
profilerHasNumMatchingEntriesOrThrow({
    profileDB: primaryDB,
    filter: {ns: target.getFullName(), "command.listIndexes": target.getName()},
    numExpectedMatches: 1
});

// Force a refresh on rs0. This is necessary because MongoS will get StaleDbVersion upon sending
// the agg request below, causing it to retry the agg command from the top and thus send
// listIndexes to the primary shard twice.
assert.commandWorked(st.rs0.getPrimary().getDB('test').adminCommand(
    {_flushDatabaseCacheUpdates: 'test', syncFromConfig: true}));

// Test that the maxTimeMS value is used for both the listIndexes command for uniqueKey
// validation as well as the $merge aggregation itself.
(function testMaxTimeMS() {
    assert.commandWorked(source.runCommand("aggregate", {
        pipeline: [{
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: Object.keys(shardKey)
            }
        }],
        cursor: {},
        maxTimeMS: maxTimeMS
    }));

    // Verify the profile entry for the aggregate on the source collection.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryDB,
        filter: {
            ns: source.getFullName(),
            "command.aggregate": source.getName(),
            "command.maxTimeMS": maxTimeMS
        },
        numExpectedMatches: 1
    });

    // The listIndexes command should be sent to the primary shard only. Note that the
    // maxTimeMS will *not* show up in the profiler since the parameter is used as a timeout for
    // the remote command vs. part of the command itself.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryDB,
        filter: {ns: target.getFullName(), "command.listIndexes": target.getName()},
        numExpectedMatches: 2
    });
})();

(function testTimeout() {
    // Configure the "maxTimeAlwaysTimeOut" fail point on the primary shard, which forces
    // mongod to throw if it receives an operation with a max time.
    assert.commandWorked(primaryDB.getSiblingDB("admin").runCommand(
        {configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}));

    // Test that the $merge correctly fails when the maxTimeMS is exceeded.
    const res = source.runCommand("aggregate", {
        pipeline: [{
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: Object.keys(shardKey)
            }
        }],
        cursor: {},
        maxTimeMS: maxTimeMS
    });
    assert.commandFailedWithCode(
        res,
        ErrorCodes.MaxTimeMSExpired,
        "expected aggregate to fail with code " + ErrorCodes.MaxTimeMSExpired +
            " due to maxTimeAlwaysTimeOut fail point, but instead got: " + tojson(res));

    // The actual aggregate should not be in the profiler since the initial listIndexes should
    // have timed out.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryDB,
        filter: {
            ns: source.getFullName(),
            "command.aggregate": source.getName(),
            "command.maxTimeMS": maxTimeMS
        },
        numExpectedMatches: 1
    });

    // Verify that there is an additional listIndexes profiler entry on the primary shard.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryDB,
        filter: {ns: target.getFullName(), "command.listIndexes": target.getName()},
        numExpectedMatches: 3
    });

    assert.commandWorked(primaryDB.getSiblingDB("admin").runCommand(
        {configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}));
})();

// Test that setting a read preference on the $merge also applies to the listIndexes
// command.
(function testReadPreference() {
    const secondaryDB = st.rs0.getSecondary().getDB("test");
    assert.commandWorked(secondaryDB.setProfilingLevel(2));

    assert.commandWorked(source.runCommand("aggregate", {
        pipeline: [{
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: Object.keys(shardKey)
            }
        }],
        cursor: {},
        $readPreference: {mode: "secondary"}
    }));

    // Verify that the profiler on the secondary includes an entry for the listIndexes.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: secondaryDB,
        filter: {ns: target.getFullName(), "command.listIndexes": target.getName()},
        numExpectedMatches: 1
    });

    // Verify that the primary shard does *not* have an additional listIndexes profiler entry.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryDB,
        filter: {ns: target.getFullName(), "command.listIndexes": target.getName()},
        numExpectedMatches: 3
    });

    profilerHasNumMatchingEntriesOrThrow({
        profileDB: secondaryDB,
        filter: {
            ns: source.getFullName(),
            "command.aggregate": source.getName(),
            "command.$readPreference": {mode: "secondary"},
        },
        numExpectedMatches: 1
    });

    // Test that $out cannot be run against a secondary since it writes directly to a local temp
    // collection.
    assert.commandFailedWithCode(
        source.runCommand(
            "aggregate",
            {pipeline: [{$out: "non_existent"}], cursor: {}, $readPreference: {mode: "secondary"}}),
        16994,
        "Expected $out to fail to create the temp collection.");
})();

st.stop();
})();
