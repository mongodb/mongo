/**
 * Tests basic support for sampling nested aggregate queries (i.e. inside $lookup, $graphLookup,
 * $unionWith) against an unsharded collection on a sharded cluster.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/sample_nested_agg_queries_common.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

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
// aggregate queries will be issued.
assert.commandWorked(mongosDB.getCollection(localCollName).insert([{a: 0}]));

// Set up the foreign collection.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(mongosDB.createCollection(foreignCollName));

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: foreignNs, mode: "full", sampleRate: 1000}));
const foreignCollUUid = QuerySamplingUtil.getCollectionUuid(mongosDB, foreignCollName);
QuerySamplingUtil.waitForActiveSamplingShardedCluster(
    st, foreignNs, foreignCollUUid, {skipMongoses: true});

// The foreign collection is unsharded so all documents are on the primary shard.
const shardNames = [st.rs0.name];

for (let {name,
          makeOuterPipelineFunc,
          requireShardToRouteFunc,
          supportCustomPipeline} of outerAggTestCases) {
    const requireShardToRoute =
        requireShardToRouteFunc(mongosDB, foreignCollName, false /* isShardedColl */);
    if (supportCustomPipeline) {
        for (let {makeInnerPipelineFunc, containInitialFilter} of innerAggTestCases) {
            const filter0 = {x: 1, name};
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter0,
                                    shardNames,
                                    false /* explain */,
                                    requireShardToRoute);

            const filter1 = {x: 2, name};
            testCustomInnerPipeline(makeOuterPipelineFunc,
                                    makeInnerPipelineFunc,
                                    containInitialFilter,
                                    st,
                                    dbName,
                                    localCollName,
                                    foreignCollName,
                                    filter1,
                                    shardNames,
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
