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
function runCmd(makeCmdObjFunc, filter, explain) {
    const collation = QuerySamplingUtil.generateRandomCollation();
    const originalCmdObj = makeCmdObjFunc(filter, collation);
    const cmdName = Object.keys(originalCmdObj)[0];

    assert.commandWorked(mongosDB.runCommand(explain ? {explain: originalCmdObj} : originalCmdObj));
    // 'explain' queries should not get sampled.
    if (!explain) {
        expectedSampledQueryDocs.push(
            {filter: {"cmd.filter": filter}, cmdName, cmdObj: {filter, collation}, shardNames});
    }
}

{
    // Run find commands.
    const makeCmdObjFunc = (filter, collation) => {
        return {
            find: collName,
            filter,
            collation,
        };
    };

    const filter0 = {x: 1};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter1 = {x: 2};
    runCmd(makeCmdObjFunc, filter1, true /* explain */);
}

{
    // Run count commands.
    const makeCmdObjFunc = (filter, collation) => {
        return {
            count: collName,
            query: filter,
            collation,
        };
    };

    const filter0 = {x: 3};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter1 = {x: 4};
    runCmd(makeCmdObjFunc, filter1, true /* explain */);
}

{
    // Run distinct commands.
    const makeCmdObjFunc = (filter, collation) => {
        return {
            distinct: collName,
            key: "x",
            query: filter,
            collation,
        };
    };

    const filter0 = {x: 5};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter1 = {x: 6};
    runCmd(makeCmdObjFunc, filter1, true /* explain */);
}

const cmdNames = ["find", "count", "distinct"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
