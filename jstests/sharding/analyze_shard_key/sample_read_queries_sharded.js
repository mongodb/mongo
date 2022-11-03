/**
 * Tests basic support for sampling read queries against a sharded collection on a sharded cluster.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const st = new ShardingTest({
    shards: 3,
    rs: {nodes: 2, setParameter: {queryAnalysisWriterIntervalSecs: 1}},
    mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs: 1}}
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);

// Make the collection have two chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 1000}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.name}));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
QuerySamplingUtil.waitForActiveSampling(st.s);

const expectedSampledQueryDocs = [];

// Make each read below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

{
    // Run find commands.
    const cmdName = "find";

    function runFindCmd(filter, shardNames) {
        const collation = QuerySamplingUtil.generateRandomCollation();
        const originalCmdObj = {
            find: collName,
            filter,
            collation,
        };
        assert.commandWorked(mongosDB.runCommand(originalCmdObj));
        expectedSampledQueryDocs.push({
            filter: {"cmd.filter": filter},
            cmdName: cmdName,
            cmdObj: {filter, collation},
            shardNames
        });
    }

    const filter0 = {x: 1};
    const shardNames0 = [st.rs1.name];
    runFindCmd(filter0, shardNames0);

    const filter1 = {x: {$gte: 2}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runFindCmd(filter1, shardNames1);
}

{
    // Run count commands.
    const cmdName = "count";

    function runCountCmd(filter, shardNames) {
        const collation = QuerySamplingUtil.generateRandomCollation();
        const originalCmdObj = {
            count: collName,
            query: filter,
            collation,
        };

        assert.commandWorked(mongosDB.runCommand(originalCmdObj));

        expectedSampledQueryDocs.push({
            filter: {"cmd.filter": filter},
            cmdName: cmdName,
            cmdObj: {filter, collation},
            shardNames
        });
    }

    const filter0 = {x: 3};
    const shardNames0 = [st.rs1.name];
    runCountCmd(filter0, shardNames0);

    const filter1 = {x: {$gte: 4}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCountCmd(filter1, shardNames1);
}

{
    // Run distinct commands.
    const cmdName = "distinct";

    function runDistinctCmd(filter, shardNames) {
        const collation = QuerySamplingUtil.generateRandomCollation();
        const originalCmdObj = {
            distinct: collName,
            key: "x",
            query: filter,
            collation,
        };

        assert.commandWorked(mongosDB.runCommand(originalCmdObj));

        expectedSampledQueryDocs.push({
            filter: {"cmd.filter": filter},
            cmdName: cmdName,
            cmdObj: {filter, collation},
            shardNames
        });
    }

    const filter0 = {x: 5};
    const shardNames0 = [st.rs1.name];
    runDistinctCmd(filter0, shardNames0);

    const filter1 = {x: {$gte: 6}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runDistinctCmd(filter1, shardNames1);
}

const cmdNames = ["find", "count", "distinct"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
