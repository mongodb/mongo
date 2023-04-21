/**
 * Tests basic support for sampling write queries against an unsharded collection on a sharded
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

// Make each write below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

{
    // Perform some updates.
    assert.commandWorked(mongosColl.insert([{x: 1, y: 1, z: [1, 0, 1]}, {x: 2, y: 2, z: [2]}]));

    const cmdName = "update";
    const updateOp0 = {
        q: {x: 1},
        u: {$mul: {y: 10}, $set: {"z.$[element]": 10}},
        arrayFilters: [{"element": {$gte: 1}}],
        multi: false,
        upsert: false,
        collation: QuerySamplingUtil.generateRandomCollation(),
    };
    const diff0 = {y: 'u', z: 'u'};
    const updateOp1 = {
        q: {x: 2},
        u: [{$set: {y: 20, w: 200}}],
        c: {var0: 1},
        multi: true,
    };
    const diff1 = {y: 'u', w: 'i'};
    const originalCmdObj = {
        update: collName,
        updates: [updateOp0, updateOp1],
        let : {var1: 1},
    };

    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.neq(mongosColl.findOne({x: 1, y: 10, z: [10, 0, 10]}), null);
    assert.neq(mongosColl.findOne({x: 2, y: 20, z: [2], w: 200}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": updateOp0.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {updates: [updateOp0]}),
        diff: diff0,
        shardNames
    });
    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": updateOp1.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {updates: [updateOp1]}),
        diff: diff1,
        shardNames
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand(
        {explain: {update: collName, updates: [{q: {x: 101}, u: [{$set: {y: 101}}]}]}}));
}

{
    // Perform some deletes.
    assert.commandWorked(mongosColl.insert([{x: 3}, {x: 4}]));

    const cmdName = "delete";
    const deleteOp0 = {
        q: {x: 3},
        limit: 1,
        collation: QuerySamplingUtil.generateRandomCollation(),
    };
    const deleteOp1 = {q: {x: 4}, limit: 0};
    const originalCmdObj = {
        delete: collName,
        deletes: [deleteOp0, deleteOp1],

    };

    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.eq(mongosColl.findOne({x: 3}), null);
    assert.eq(mongosColl.findOne({x: 4}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.deletes.0.q": deleteOp0.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp0]}),
        shardNames
    });
    expectedSampledQueryDocs.push({
        filter: {"cmd.deletes.0.q": deleteOp1.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp1]}),
        shardNames
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {delete: collName, deletes: [{q: {x: 301}, limit: 1}]}}));
}

{
    // Perform some findAndModify.
    assert.commandWorked(mongosColl.insert([{x: 5, y: 5, z: [5, 0, 5]}]));

    const cmdName = "findAndModify";
    const originalCmdObj = {
        findAndModify: collName,
        query: {x: 5},
        update: {$mul: {y: 10}, $set: {"z.$[element]": 50}},
        arrayFilters: [{"element": {$gte: 5}}],
        sort: {_id: 1},
        collation: QuerySamplingUtil.generateRandomCollation(),
        new: true,
        upsert: false,
        let : {var0: 1}
    };
    const diff = {y: 'u', z: 'u'};

    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.neq(mongosColl.findOne({x: 5, y: 50, z: [50, 0, 50]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj),
        diff,
        shardNames
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand(
        {explain: {findAndModify: collName, query: {x: 501}, update: {$set: {y: 501}}}}));
}

const cmdNames = ["update", "delete", "findAndModify"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
