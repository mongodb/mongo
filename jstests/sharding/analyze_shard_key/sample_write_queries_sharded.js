/**
 * Tests basic support for sampling write queries against a sharded collection on a sharded cluster.
 *
 * @tags: [requires_fcv_70]
 */
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";

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

// Make the collection have two chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
assert.commandWorked(mongosDB.createCollection(collName));
assert.commandWorked(mongosColl.createIndex({x: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 1000}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));
const collectionUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);

assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1000}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collectionUuid);

const expectedSampledQueryDocs = [];

// Make each write below have a unique filter and use that to look up the corresponding
// config.sampledQueries document later.

(function runUpdates() {
    // Perform some updates.
    assert.commandWorked(
        mongosColl.insert([
            // The doc below is on shard0.
            {x: -1, y: -1, z: [-1]},
            // The docs below are on shard1.
            {x: 1, y: 1, z: [1, 0, 1]},
            {x: 2, y: 2, z: [2]},
            // The doc below is on shard2.
            {x: 1002, y: 2, z: [2]},
        ]),
    );

    const collation = QuerySamplingUtil.generateRandomCollation();
    const letField = {var1: {$literal: 1}};
    // When UWE is enabled, shards receive bulkWrite commands instead, so the test expectation changes accordingly.
    const cmdName = uweEnabled ? "bulkWrite" : "update";
    const singleUpdate0 = {
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
        : {"cmd.updates.0.q": singleUpdate0.q};
    const cmdObj0 = uweEnabled
        ? {bulkWrite: 1, ops: [bulkUpdateOp0], let: letField}
        : {update: collName, updates: [singleUpdate0], let: letField};
    const diff0 = {y: "u", z: "u"};
    const shardNames0 = [st.rs1.name];

    const singleUpdateOp1 = {
        q: {x: {$gte: 2}},
        u: [{$set: {y: 20, w: 200}}],
        c: {var0: 1},
        multi: true,
    };
    const bulkUpdateOp1 = {
        update: 0,
        filter: {x: {$gte: 2}},
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
    const shardNames1 = [st.rs1.name, st.rs2.name];

    const originalCmdObj0 = {
        update: collName,
        updates: [singleUpdate0, singleUpdateOp1],
        let: {var1: 1},
    };

    // Use a transaction, otherwise updateOp1 would get routed to all shards.
    const session = st.s.startSession();
    withTxnAndAutoRetryOnMongos(session, () => {
        const sessionDB = session.getDatabase(dbName);
        assert.commandWorked(sessionDB.runCommand(originalCmdObj0));
    });
    assert.neq(mongosColl.findOne({x: 1, y: 10, z: [10, 0, 10]}), null);
    assert.neq(mongosColl.findOne({x: 2, y: 20, z: [2], w: 200}), null);
    assert.neq(mongosColl.findOne({x: 1002, y: 20, z: [2], w: 200}), null);

    expectedSampledQueryDocs.push({
        filter: updateOp0Filter,
        cmdName: cmdName,
        cmdObj: cmdObj0,
        diff: diff0,
        shardNames: shardNames0,
    });
    expectedSampledQueryDocs.push({
        filter: updateOp1Filter,
        cmdName: cmdName,
        cmdObj: cmdObj1,
        diff: diff1,
        shardNames: shardNames1,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {update: collName, updates: [{q: {x: 101}, u: [{$set: {y: 101}}]}]}}),
    );
    assert.commandWorked(
        mongosDB.runCommand({
            explain: {update: collName, updates: [{q: {x: {$gte: 102}}, u: {$set: {y: 102}}, multi: true}]},
        }),
    );

    // TODO SERVER-104122: Enable when 'WouldChangeOwningShard' writes are supported.
    if (uweEnabled) {
        return;
    }
    // This is a WouldChangeOwningShard update. It causes the document to move from shard0 to
    // shard1.
    const singleUpdateOp2 = {
        q: {x: -1},
        u: {$inc: {x: 1000}, $set: {v: -1}},
        multi: false,
        collation: QuerySamplingUtil.generateRandomCollation(),
    };
    const diff2 = {x: "u", v: "i"};
    const shardNames2 = [st.rs0.name];

    const originalCmdObj1 = {
        update: collName,
        updates: [singleUpdateOp2],
        let: {var1: 1},
    };

    assert.commandWorked(
        mongosDB.runCommand(Object.assign({}, originalCmdObj1, {lsid: {id: UUID()}, txnNumber: NumberLong(1)})),
    );
    assert.neq(mongosColl.findOne({x: 999, y: -1, z: [-1], v: -1}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.updates.0.q": singleUpdateOp2.q},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj1, {updates: [singleUpdateOp2], let: letField}),
        diff: diff2,
        shardNames: shardNames2,
    });
})();

(function runDeletes() {
    // Perform some deletes.
    assert.commandWorked(
        mongosColl.insert([
            // The docs below are on shard1.
            {x: 3},
            {x: 4},
            // The docs below are on shard2.
            {x: 1004},
        ]),
    );

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
    const shardNames0 = [st.rs1.name];

    const singleDeleteOp1 = {q: {x: {$gte: 4}}, limit: 0};
    const bulkDeleteOp1 = {filter: {x: {$gte: 4}}, multi: true};
    const deleteOp1Filter = uweEnabled
        ? {"cmd.ops.0.filter": bulkDeleteOp1.filter}
        : {"cmd.deletes.0.q": singleDeleteOp1.q};
    const cmdObj1 = uweEnabled ? {bulkWrite: 1, ops: [bulkDeleteOp1]} : {delete: collName, deletes: [singleDeleteOp1]};
    const shardNames1 = [st.rs1.name, st.rs2.name];

    const originalCmdObj = {delete: collName, deletes: [singleDeleteOp0, singleDeleteOp1]};

    // Use a transaction, otherwise deleteOp1 would get routed to all shards.
    const session = st.s.startSession();
    withTxnAndAutoRetryOnMongos(session, () => {
        const sessionDB = session.getDatabase(dbName);
        assert.commandWorked(sessionDB.runCommand(originalCmdObj));
    });
    assert.eq(mongosColl.findOne({x: 3}), null);
    assert.eq(mongosColl.findOne({x: 4}), null);

    expectedSampledQueryDocs.push({
        filter: deleteOp0Filter,
        cmdName: cmdName,
        cmdObj: cmdObj0,
        shardNames: shardNames0,
    });
    expectedSampledQueryDocs.push({
        filter: deleteOp1Filter,
        cmdName: cmdName,
        cmdObj: cmdObj1,
        shardNames: shardNames1,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(mongosDB.runCommand({explain: {delete: collName, deletes: [{q: {x: 301}, limit: 1}]}}));
    assert.commandWorked(
        mongosDB.runCommand({explain: {delete: collName, deletes: [{q: {x: {$gte: 401}}, limit: 0}]}}),
    );
})();

(function runFindAndModify() {
    // Perform some findAndModify.
    assert.commandWorked(
        mongosColl.insert([
            // The doc below is on shard0.
            {x: -5, y: -5, z: [-5, 0, -5]},
            // The doc below is on shard1.
            {x: 6, y: 6, z: [6, 0, 6]},
        ]),
    );

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
        let: {var0: 1},
    };
    const diff0 = {y: "u", z: "u"};
    const shardNames0 = [st.rs0.name];

    assert.commandWorked(mongosDB.runCommand(originalCmdObj0));
    assert.neq(mongosColl.findOne({x: -5, y: -50, z: [-50, 0, -50]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj0.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj0, {let: {var0: {$literal: 1}}}),
        diff: diff0,
        shardNames: shardNames0,
    });

    // 'explain' queries should not get sampled.
    assert.commandWorked(
        mongosDB.runCommand({explain: {findAndModify: collName, query: {x: 501}, update: {$set: {y: 501}}}}),
    );

    // TODO SERVER-104122: Enable when 'WouldChangeOwningShard' writes are supported.
    if (uweEnabled) {
        return;
    }
    // This is a WouldChangeOwningShard update. It causes the document to move from shard1 to
    // shard2.
    const originalCmdObj1 = {
        findAndModify: collName,
        query: {x: 6},
        update: {$inc: {x: 1000}, $mul: {y: -1}},
        collation: QuerySamplingUtil.generateRandomCollation(),
        upsert: false,
        let: {var0: 1},
    };
    const diff1 = {x: "u", y: "u"};
    const shardNames1 = [st.rs1.name];

    assert.commandWorked(
        mongosDB.runCommand(Object.assign({}, originalCmdObj1, {lsid: {id: UUID()}, txnNumber: NumberLong(1)})),
    );
    assert.neq(mongosColl.findOne({x: 1006, y: -6, z: [6, 0, 6]}), null);

    expectedSampledQueryDocs.push({
        filter: {"cmd.query": originalCmdObj1.query},
        cmdName: cmdName,
        cmdObj: Object.assign({}, originalCmdObj1, {let: {var0: {$literal: 1}}}),
        diff: diff1,
        shardNames: shardNames1,
    });
})();

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
