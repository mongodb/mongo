/**
 * Tests basic support for sampling read queries against an unsharded collection on a sharded
 * cluster.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, setParameter: {queryAnalysisWriterIntervalSecs: 1}},
    mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs: 1}}
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongosDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(mongosDB.createCollection(collName));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
QuerySamplingUtil.waitForActiveSampling(st.s);

const expectedSampledQueryDocs = [];
// This is an unsharded collection so all documents are on the primary shard.
const shardNames = [st.rs0.name];

// Make each read below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

{
    // Run a find command.
    const cmdName = "find";
    const filter = {x: 1};
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

{
    // Run a count command.
    const cmdName = "count";
    const filter = {x: 2};
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

{
    // Run a distinct command.
    const cmdName = "distinct";
    const filter = {x: 3};
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

const cmdNames = ["find", "count", "distinct"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
