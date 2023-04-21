/**
 * Tests basic support for sampling read queries against a sharded collection on a sharded cluster.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisWriterIntervalSecs = 1;
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const st = new ShardingTest({
    shards: 3,
    rs: {
        nodes: 2,
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            logComponentVerbosity: tojson({sharding: 2})
        }
    },
    mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}}
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);

// Make the collection have three chunks:
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
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collectionUuid);

const expectedSampledQueryDocs = [];

// Make each read below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.
function runCmd(makeCmdObjFunc, filter, shardNames, explain, expectFilterCaptured = true) {
    const collation = QuerySamplingUtil.generateRandomCollation();
    const originalCmdObj = makeCmdObjFunc(filter, collation);
    const cmdName = Object.keys(originalCmdObj)[0];

    assert.commandWorked(mongosDB.runCommand(explain ? {explain: originalCmdObj} : originalCmdObj));
    // 'explain' queries should not get sampled.
    if (!explain) {
        const expectedFilter = expectFilterCaptured ? filter : {};
        expectedSampledQueryDocs.push({
            filter: {"cmd.filter": expectedFilter},
            cmdName,
            cmdObj: {filter: expectedFilter, collation},
            shardNames
        });
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
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 2}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 3};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 4}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
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

    const filter0 = {x: 5};
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 6}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 7};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 8}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
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

    const filter0 = {x: 9};
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 10}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 11};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 12}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
}

{
    // Run aggregate commands with filter in the first stage ($match).
    const makeCmdObjFunc = (filter, collation) => {
        return {aggregate: collName, pipeline: [{$match: filter}], collation, cursor: {}};
    };

    const filter0 = {x: 13};
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 14}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 15};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 16}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
}

{
    // Run aggregate commands with filter in the first stage ($geoNear).
    const makeCmdObjFunc = (filter, collation) => {
        const geoNearStage = {
            $geoNear: {near: {type: "Point", coordinates: [1, 1]}, distanceField: "dist"}
        };
        if (filter !== null) {
            geoNearStage.$geoNear.query = filter;
        }
        return {aggregate: collName, pipeline: [geoNearStage], collation, cursor: {}};
    };

    assert.commandWorked(mongosColl.createIndex({x: "2dsphere"}));

    const filter0 = {x: 17};
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 18}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 19};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 20}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
}

{
    // Run aggregate commands with filter in a non-first but moveable stage ($match).
    const makeCmdObjFunc = (filter, collation) => {
        return {
            aggregate: collName,
            pipeline: [{$sort: {x: -1}}, {$match: filter}],
            collation,
            cursor: {}
        };
    };

    const filter0 = {x: 21};
    const shardNames0 = [st.rs1.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */);

    const filter1 = {x: {$gte: 22}};
    const shardNames1 = [st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter1, shardNames1, false /* explain */);

    const filter2 = {x: 23};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */);

    const filter3 = {x: {$gte: 24}};
    const shardNames3 = [];
    runCmd(makeCmdObjFunc, filter3, shardNames3, true /* explain */);
}

{
    // Run aggregate commands with filter in a non-first and non-moveable stage ($match).
    const makeCmdObjFunc = (filter, collation) => {
        return {
            aggregate: collName,
            pipeline: [{$_internalInhibitOptimization: {}}, {$match: filter}],
            collation,
            cursor: {}
        };
    };

    // The filter can't be moved up so the commands below don't have an initial filter.
    const expectFilterCaptured = false;

    const filter0 = {x: 25};
    const shardNames0 = [st.rs0.name, st.rs1.name, st.rs2.name];
    runCmd(makeCmdObjFunc, filter0, shardNames0, false /* explain */, expectFilterCaptured);

    const filter1 = {x: 26};
    const shardNames1 = [];
    runCmd(makeCmdObjFunc, filter1, shardNames1, true /* explain */, expectFilterCaptured);

    const filter2 = {x: {$gte: 27}};
    const shardNames2 = [];
    runCmd(makeCmdObjFunc, filter2, shardNames2, true /* explain */, expectFilterCaptured);
}

const cmdNames = ["find", "count", "distinct", "aggregate"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
