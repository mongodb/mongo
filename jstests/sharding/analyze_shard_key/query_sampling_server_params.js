/**
 * Tests that the periods of the periodic jobs for refreshing query sampling configurations and
 * writing sampled queries and diffs are configurable in runtime.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";

const numNodesPerRS = 2;
// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

function testQuerySampling(conn, {rst, st}) {
    const dbName = "testDb";
    const collName = "testColl";
    const numDocs = 1000;
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({x: i});
    }
    assert.commandWorked(coll.insert(docs, {writeConcern}));
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1000}));
    QuerySamplingUtil.waitForActiveSampling(ns, collUuid, {rst, st});

    assert.commandWorked(coll.update({x: 1}, {$mul: {x: -1}}));
    assert.neq(coll.findOne({x: -1}), null);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
    QuerySamplingUtil.waitForInactiveSampling(ns, collUuid, {rst, st});

    assert.soon(() => {
        const res = conn.adminCommand({
            analyzeShardKey: ns,
            key: {x: 1},
            keyCharacteristics: false,
            readWriteDistribution: true
        });

        if (res.readDistribution.sampleSize.total == 0) {
            // The sampled find query has not been written to disk, so retry.
            return false;
        }
        assert.eq(res.readDistribution.sampleSize.total, 1);
        assert.eq(res.readDistribution.sampleSize.find, 1);

        if (res.writeDistribution.sampleSize.total == 0) {
            // The sampled update query has not been written to disk, so retry.
            return false;
        }
        assert.eq(res.writeDistribution.sampleSize.total, 1);
        assert.eq(res.writeDistribution.sampleSize.update, 1);

        if (res.writeDistribution.percentageOfShardKeyUpdates != 100) {
            // The sampled diff has not been written to disk, so retry.
            return false;
        }

        return true;
    });
}

// Upon startup, make periodic jobs for refreshing query sampling configurations and writing sampled
// queries and diffs have periods of 6 hours. After startup, make them have periods of 1 second. If
// the setParameter command does not update the periods correctly, the query sampling test below
// would fail.
const mongodSetParameterOptsStartup = {
    queryAnalysisWriterIntervalSecs: 6 * 3600,
    queryAnalysisSamplerConfigurationRefreshSecs: 6 * 3600,
    logComponentVerbosity: tojson({sharding: 2}),
};
const mongosSetParameterOptsStartup = {
    queryAnalysisSamplerConfigurationRefreshSecs: 6 * 3600,
    logComponentVerbosity: tojson({sharding: 3})
};

const mongodSetParameterCmdObj = {
    setParameter: 1,
    queryAnalysisWriterIntervalSecs: 1,
    queryAnalysisSamplerConfigurationRefreshSecs: 1,
};

const mongosSetParameterCmdObj = {
    setParameter: 1,
    queryAnalysisSamplerConfigurationRefreshSecs: 1,
};

{
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: numNodesPerRS, setParameter: mongodSetParameterOptsStartup},
        mongosOptions: {setParameter: mongosSetParameterOptsStartup}
    });

    assert.commandWorked(st.s.adminCommand(mongosSetParameterCmdObj));
    st.rs0.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(mongodSetParameterCmdObj));
    });

    testQuerySampling(st.s, {st});
    st.stop();
}

{
    const rst = new ReplSetTest(
        {nodes: numNodesPerRS, nodeOptions: {setParameter: mongodSetParameterOptsStartup}});
    rst.startSet();
    rst.initiate();

    rst.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(mongodSetParameterCmdObj));
    });

    testQuerySampling(rst.getPrimary(), {rst});
    rst.stopSet();
}
