/**
 * Tests basic support for sampling write queries against an unsharded collection on a sharded
 * cluster.
 *
 * @tags: [requires_fcv_70]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";

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
            logComponentVerbosity: tojson({sharding: 2}),
        },
    },
    mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}},
});

const uweEnabled = isUweEnabled(st.s);

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
assert.commandWorked(mongosDB.createCollection(collName));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1000}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collectionUuid);

const expectedSampledQueryDocs = [];
// This is an unsharded collection so all documents are on the primary shard.
const shardNames = [st.rs0.name];

// Make each write below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

{
    // Perform some updates.
    assert.commandWorked(
        mongosColl.insert([
            {x: 1, y: 1, z: [1, 0, 1]},
            {x: 2, y: 2, z: [2]},
        ]),
    );

    const collation = QuerySamplingUtil.generateRandomCollation();
    const letField = {var1: {$literal: 1}};
    // When UWE is enabled, shards receive bulkWrite commands instead, so the test expectation changes accordingly.
    const cmdName = uweEnabled ? "bulkWrite" : "update";
    const singleUpdateOp0 = {
        q: {x: 1},
        u: {$mul: {y: 10}, $set: {"z.$[element]": 10}},
        arrayFilters: [{"element": {$gte: 1}}],
        multi: false,
        upsert: false,
        collation,
    };
    const bulkUpdateOp0 = {
        update: 0,
        filter: {x: 1},
        updateMods: {$mul: {y: 10}, $set: {"z.$[element]": 10}},
        arrayFilters: [{"element": {$gte: 1}}],
        multi: false,
        upsert: false,
        collation,
    };
    const updateOp0Filter = uweEnabled
        ? {"cmd.ops.0.filter": bulkUpdateOp0.filter}
        : {"cmd.updates.0.q": singleUpdateOp0.q};
    const cmdObj0 = uweEnabled
        ? {bulkWrite: 1, ops: [bulkUpdateOp0], let: letField}
        : {update: collName, updates: [singleUpdateOp0], let: letField};
    const diff0 = {y: "u", z: "u"};

    const singleUpdateOp1 = {
        q: {x: 2},
        u: [{$set: {y: 20, w: 200}}],
        c: {var0: 1},
        multi: true,
    };
    const bulkUpdateOp1 = {
        update: 0,
        filter: {x: 2},
        updateMods: [{$set: {y: 20, w: 200}}],
        constants: {var0: 1},
        multi: true,
    };
    const updateOp1Filter = uweEnabled
        ? {"cmd.ops.0.filter": bulkUpdateOp1.filter}
        : {"cmd.updates.0.q": singleUpdateOp1.q};
    const cmdObj1 = uweEnabled
        ? {bulkWrite: 1, ops: [bulkUpdateOp1], let: letField}
        : {update: collName, updates: [singleUpdateOp1], let: letField};
    const diff1 = {y: "u", w: "i"};

    const originalCmdObj = {
        update: collName,
        updates: [singleUpdateOp0, singleUpdateOp1],
        let: {var1: 1},
    };
    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.neq(mongosColl.findOne({x: 1, y: 10, z: [10, 0, 10]}), null);
    assert.neq(mongosColl.findOne({x: 2, y: 20, z: [2], w: 200}), null);

    expectedSampledQueryDocs.push({
        filter: updateOp0Filter,
        cmdName: cmdName,
        cmdObj: cmdObj0,
        diff: diff0,
        shardNames,
    });
    expectedSampledQueryDocs.push({
        filter: updateOp1Filter,
        cmdName: cmdName,
        cmdObj: cmdObj1,
        diff: diff1,
        shardNames,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {update: collName, updates: [{q: {x: 101}, u: [{$set: {y: 101}}]}]}}),
    );
}

{
    // Perform some deletes.
    assert.commandWorked(mongosColl.insert([{x: 3}, {x: 4}]));

    const collation = QuerySamplingUtil.generateRandomCollation();
    // When UWE is enabled, shards receive bulkWrite commands instead, so the test expectation changes accordingly.
    const cmdName = uweEnabled ? "bulkWrite" : "delete";
    const singleDeleteOp0 = {
        q: {x: 3},
        limit: 1,
        collation,
    };
    const bulkDeleteOp0 = {
        filter: {x: 3},
        multi: false,
        collation,
    };
    const deleteOp0Filter = uweEnabled
        ? {"cmd.ops.0.filter": bulkDeleteOp0.filter}
        : {"cmd.deletes.0.q": singleDeleteOp0.q};
    const cmdObj0 = uweEnabled ? {bulkWrite: 1, ops: [bulkDeleteOp0]} : {delete: collName, deletes: [singleDeleteOp0]};

    const singleDeleteOp1 = {q: {x: 4}, limit: 0};
    const bulkDeleteOp1 = {filter: {x: 4}, multi: true};
    const deleteOp1Filter = uweEnabled
        ? {"cmd.ops.0.filter": bulkDeleteOp1.filter}
        : {"cmd.deletes.0.q": singleDeleteOp1.q};
    const cmdObj1 = uweEnabled ? {bulkWrite: 1, ops: [bulkDeleteOp1]} : {delete: collName, deletes: [singleDeleteOp1]};

    const originalCmdObj = {
        delete: collName,
        deletes: [singleDeleteOp0, singleDeleteOp1],
    };
    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.eq(mongosColl.findOne({x: 3}), null);
    assert.eq(mongosColl.findOne({x: 4}), null);

    expectedSampledQueryDocs.push({
        filter: deleteOp0Filter,
        cmdName: cmdName,
        cmdObj: cmdObj0,
        shardNames,
    });
    expectedSampledQueryDocs.push({
        filter: deleteOp1Filter,
        cmdName: cmdName,
        cmdObj: cmdObj1,
        shardNames,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand({explain: {delete: collName, deletes: [{q: {x: 301}, limit: 1}]}}));
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
        let: {var0: {$literal: "$nonExistentFieldName"}},
    };
    const diff = {y: "u", z: "u"};

    assert.commandWorked(mongosDB.runCommand(originalCmdObj));
    assert.neq(mongosColl.findOne({x: 5, y: 50, z: [50, 0, 50]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj),
        diff,
        shardNames,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {findAndModify: collName, query: {x: 501}, update: {$set: {y: 501}}}}),
    );
}

const cmdNames = uweEnabled ? ["bulkWrite", "findAndModify"] : ["update", "delete", "findAndModify"];
QuerySamplingUtil.assertSoonSampledQueryDocumentsAcrossShards(
    st,
    ns,
    collectionUuid,
    cmdNames,
    expectedSampledQueryDocs,
);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

st.stop();
