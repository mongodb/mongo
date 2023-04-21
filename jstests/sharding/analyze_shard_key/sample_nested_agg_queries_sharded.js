/**
 * Tests basic support for sampling nested aggregate queries (i.e. ones inside $lookup,
 * $graphLookup, $unionWith) against a sharded collection on a sharded cluster.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/sample_nested_agg_queries_common.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

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
    // Disable query sampling on mongos to verify that the nested aggregate queries are sampled by
    // the shard that routes them.
    mongosOptions:
        {setParameter: {"failpoint.disableQueryAnalysisSampler": tojson({mode: "alwaysOn"})}}
});

const dbName = "testDb";
const localCollName = "testLocalColl";
const foreignCollName = "testForeignColl";
const foreignNs = dbName + "." + foreignCollName;
const mongosDB = st.s.getDB(dbName);

// Set up the local collection. It needs to have at least one document. Otherwise, no nested
// aggregate queries would be issued.
assert.commandWorked(mongosDB.getCollection(localCollName).insert([{a: 0}]));

// Set up the foreign collection. Make it have three chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 1000}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: 1000}, to: st.shard2.name}));

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: foreignNs, mode: "full", sampleRate: 1000}));
const foreignCollUUid = QuerySamplingUtil.getCollectionUuid(mongosDB, foreignCollName);
QuerySamplingUtil.waitForActiveSamplingShardedCluster(
    st, foreignNs, foreignCollUUid, {skipMongoses: true});

for (let {name,
          makeOuterPipelineFunc,
          requireShardToRouteFunc,
          supportCustomPipeline} of outerAggTestCases) {
    const requireShardToRoute =
        requireShardToRouteFunc(mongosDB, foreignCollName, true /* isShardedColl */);
    if (supportCustomPipeline) {
        for (let {makeInnerPipelineFunc, containInitialFilter} of innerAggTestCases) {
            const filter0 = {x: 1, name};
            // If the aggregation doesn't have an initial filter, it would be routed to all shards.
            const shardNames0 =
                containInitialFilter ? [st.rs1.name] : [st.rs0.name, st.rs1.name, st.rs2.name];
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter0,
                                    shardNames0,
                                    false /* explain */,
                                    requireShardToRoute);

            const filter1 = {x: {$gte: 2}, name};
            // If the aggregation doesn't have an initial filter, it would be routed to all shards.
            const shardNames1 = containInitialFilter ? [st.rs1.name, st.rs2.name]
                                                     : [st.rs0.name, st.rs1.name, st.rs2.name];
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter1,
                                    shardNames1,
                                    false /* explain */,
                                    requireShardToRoute);

            const filter2 = {x: 3, name};
            const shardNames2 = [];
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter2,
                                    shardNames2,
                                    true /* explain */,
                                    requireShardToRoute);

            const filter3 = {x: {$gte: 4}, name};
            const shardNames3 = [];
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter3,
                                    shardNames3,
                                    true /* explain */,
                                    requireShardToRoute);
        }
    } else {
        testNoCustomInnerPipeline(makeOuterPipelineFunc,
                                  st,
                                  dbName,
                                  localCollName,
                                  foreignCollName,
                                  false /* explain */,
                                  requireShardToRoute);
        testNoCustomInnerPipeline(makeOuterPipelineFunc,
                                  st,
                                  dbName,
                                  localCollName,
                                  foreignCollName,
                                  true /* explain */,
                                  requireShardToRoute);
    }
}

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: foreignNs, mode: "off"}));

st.stop();
})();
