/**
 * Tests basic support for sampling write queries against a sharded collection on a sharded cluster.
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

// Make the collection have two chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
assert.commandWorked(mongosDB.createCollection(collName));
assert.commandWorked(mongosColl.createIndex({x: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 1000}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collectionUuid);

const expectedSampledQueryDocs = [];

// Make each write below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

{
    // Perform some updates.
    assert.commandWorked(mongosColl.insert([
        // The doc below is on shard0.
        {x: -1, y: -1, z: [-1]},
        // The docs below are on shard1.
        {x: 1, y: 1, z: [1, 0, 1]},
        {x: 2, y: 2, z: [2]},
        // The doc below is on shard2.
        {x: 1002, y: 2, z: [2]}
    ]));

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
    const shardNames0 = [st.rs1.name];

    const updateOp1 = {
        q: {x: {$gte: 2}},
        u: [{$set: {y: 20, w: 200}}],
        c: {var0: 1},
        multi: true,
    };
    const diff1 = {y: 'u', w: 'i'};
    const shardNames1 = [st.rs1.name, st.rs2.name];

    const originalCmdObj0 = {
        update: collName,
        updates: [updateOp0, updateOp1],
        let : {var1: 1},
    };

    // Use a transaction, otherwise updateOp1 would get routed to all shards.
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);
    assert.commandWorked(mongosDB.runCommand(Object.assign(
        {}, originalCmdObj0, {lsid, txnNumber, startTransaction: true, autocommit: false})));
    assert.commandWorked(
        mongosDB.adminCommand({commitTransaction: 1, lsid, txnNumber, autocommit: false}));
    assert.neq(mongosColl.findOne({x: 1, y: 10, z: [10, 0, 10]}), null);
    assert.neq(mongosColl.findOne({x: 2, y: 20, z: [2], w: 200}), null);
    assert.neq(mongosColl.findOne({x: 1002, y: 20, z: [2], w: 200}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": updateOp0.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj0, {updates: [updateOp0]}),
        diff: diff0,
        shardNames: shardNames0
    });
    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": updateOp1.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj0, {updates: [updateOp1]}),
        diff: diff1,
        shardNames: shardNames1
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand(
        {explain: {update: collName, updates: [{q: {x: 101}, u: [{$set: {y: 101}}]}]}}));
    assert.commandWorked(mongosDB.runCommand({
        explain:
            {update: collName, updates: [{q: {x: {$gte: 102}}, u: {$set: {y: 102}}, multi: true}]}
    }));

    // This is a WouldChangeOwningShard update. It causes the document to move from shard0 to
    // shard1.
    const updateOp2 = {
        q: {x: -1},
        u: {$inc: {x: 1000}, $set: {v: -1}},
        multi: false,
        collation: QuerySamplingUtil.generateRandomCollation(),
    };
    const diff2 = {x: 'u', v: 'i'};
    const shardNames2 = [st.rs0.name];

    const originalCmdObj1 = {
        update: collName,
        updates: [updateOp2],
        let : {var1: 1},
    };

    assert.commandWorked(mongosDB.runCommand(
        Object.assign({}, originalCmdObj1, {lsid: {id: UUID()}, txnNumber: NumberLong(1)})));
    assert.neq(mongosColl.findOne({x: 999, y: -1, z: [-1], v: -1}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": updateOp2.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj1, {updates: [updateOp2]}),
        diff: diff2,
        shardNames: shardNames2
    });
}

{
    // Perform some deletes.
    assert.commandWorked(mongosColl.insert([
        // The docs below are on shard1.
        {x: 3},
        {x: 4},
        // The docs below are on shard2.
        {x: 1004}
    ]));

    const cmdName = "delete";

    const deleteOp0 = {
        q: {x: 3},
        limit: 1,
        collation: QuerySamplingUtil.generateRandomCollation(),
    };
    const shardNames0 = [st.rs1.name];

    const deleteOp1 = {q: {x: {$gte: 4}}, limit: 0};
    const shardNames1 = [st.rs1.name, st.rs2.name];

    const originalCmdObj = {delete: collName, deletes: [deleteOp0, deleteOp1]};

    // Use a transaction, otherwise deleteOp1 would get routed to all shards.
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);
    assert.commandWorked(mongosDB.runCommand(Object.assign(
        {}, originalCmdObj, {lsid, txnNumber, startTransaction: true, autocommit: false})));
    assert.commandWorked(
        mongosDB.adminCommand({commitTransaction: 1, lsid, txnNumber, autocommit: false}));
    assert.eq(mongosColl.findOne({x: 3}), null);
    assert.eq(mongosColl.findOne({x: 4}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.deletes.0.q": deleteOp0.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp0]}),
        shardNames: shardNames0
    });
    expectedSampledQueryDocs.push({
        filter: {"cmd.deletes.0.q": deleteOp1.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp1]}),
        shardNames: shardNames1
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {delete: collName, deletes: [{q: {x: 301}, limit: 1}]}}));
    assert.commandWorked(mongosDB.runCommand(
        {explain: {delete: collName, deletes: [{q: {x: {$gte: 401}}, limit: 0}]}}));
}

{
    // Perform some findAndModify.
    assert.commandWorked(mongosColl.insert([
        // The doc below is on shard0.
        {x: -5, y: -5, z: [-5, 0, -5]},
        // The doc below is on shard1.
        {x: 6, y: 6, z: [6, 0, 6]}
    ]));

    const cmdName = "findAndModify";
    const originalCmdObj0 = {
        findAndModify: collName,
        query: {x: -5},
        update: {$mul: {y: 10}, $set: {"z.$[element]": -50}},
        arrayFilters: [{"element": {$lte: -5}}],
        sort: {_id: 1},
        collation: QuerySamplingUtil.generateRandomCollation(),
        new: true,
        upsert: false,
        let : {var0: 1}
    };
    const diff0 = {y: 'u', z: 'u'};
    const shardNames0 = [st.rs0.name];

    assert.commandWorked(mongosDB.runCommand(originalCmdObj0));
    assert.neq(mongosColl.findOne({x: -5, y: -50, z: [-50, 0, -50]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj0.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj0),
        diff: diff0,
        shardNames: shardNames0
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand(
        {explain: {findAndModify: collName, query: {x: 501}, update: {$set: {y: 501}}}}));

    // This is a WouldChangeOwningShard update. It causes the document to move from shard1 to
    // shard2.
    const originalCmdObj1 = {
        findAndModify: collName,
        query: {x: 6},
        update: {$inc: {x: 1000}, $mul: {y: -1}},
        collation: QuerySamplingUtil.generateRandomCollation(),
        upsert: false,
        let : {var0: 1}
    };
    const diff1 = {x: 'u', y: 'u'};
    const shardNames1 = [st.rs1.name];

    assert.commandWorked(mongosDB.runCommand(
        Object.assign({}, originalCmdObj1, {lsid: {id: UUID()}, txnNumber: NumberLong(1)})));
    assert.neq(mongosColl.findOne({x: 1006, y: -6, z: [6, 0, 6]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj1.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj1),
        diff: diff1,
        shardNames: shardNames1
    });
}

const cmdNames = ["update", "delete", "findAndModify"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
})();
