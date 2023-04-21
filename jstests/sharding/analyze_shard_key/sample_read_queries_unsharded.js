/**
 * Tests basic support for sampling read queries against an unsharded collection on a sharded
 * cluster.
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
    shards: 2,
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

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(mongosDB.createCollection(collName));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collectionUuid);

const expectedSampledQueryDocs = [];
// This is an unsharded collection so all documents are on the primary shard.
const shardNames = [st.rs0.name];

// Make each read below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.
function runCmd(makeCmdObjFunc, filter, explain, expectFilterCaptured = true) {
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

{
    // Run aggregate commands with filter in the first stage ($match).
    const makeCmdObjFunc = (filter, collation) => {
        return {aggregate: collName, pipeline: [{$match: filter}], collation, cursor: {}};
    };

    const filter0 = {x: 7};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter1 = {x: 8};
    runCmd(makeCmdObjFunc, filter1, true /* explain */);
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

    const filter0 = {x: 9};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter2 = {x: 10};
    runCmd(makeCmdObjFunc, filter2, true /* explain */);
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

    const filter0 = {x: 11};
    runCmd(makeCmdObjFunc, filter0, false /* explain */);

    const filter1 = {x: 12};
    runCmd(makeCmdObjFunc, filter1, true /* explain */);
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

    const filter0 = {x: 13};
    runCmd(makeCmdObjFunc, filter0, false /* explain */, expectFilterCaptured);

    const filter1 = {x: 14};
    runCmd(makeCmdObjFunc, filter1, true /* explain */, expectFilterCaptured);
}

const cmdNames = ["find", "count", "distinct", "aggregate"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
