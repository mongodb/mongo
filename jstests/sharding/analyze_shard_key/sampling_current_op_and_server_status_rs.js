/**
 * Tests the currentOp and serverStatus for query sampling on a standalone replica set.
 *
 * @tags: [requires_fcv_70]
 */

(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");
load("jstests/sharding/analyze_shard_key/libs/sampling_current_op_and_server_status_common.js");

// Make the periodic jobs for refreshing sample rates have a period of 1 second to speed up the
// test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            logComponentVerbosity: tojson({sharding: 2})
        }
    }
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const numDocs = 10;
const sampleRate = 1000;

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
// Insert initial documents.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({x: i, y: i});
}
assert.commandWorked(bulk.execute());
const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

function runCommandAndAssertCurrentOpAndServerStatus(opKind, cmdObj, oldState) {
    assert.commandWorked(primary.getDB(dbName).runCommand(cmdObj));

    let newState;
    assert.soon(() => {
        newState = getCurrentOpAndServerStatusMongod(primary);
        return assertCurrentOpAndServerStatusMongod(
            ns, opKind, oldState, newState, false /* isShardSvr */);
    });
    return newState;
}

let currentState = getCurrentOpAndServerStatusMongod(primary);
assert.eq(
    bsonWoCompare(currentState, makeInitialCurrentOpAndServerStatusMongod(0)), 0, {currentState});

// Start query sampling.
assert.commandWorked(
    primary.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: sampleRate}));
QuerySamplingUtil.waitForActiveSamplingReplicaSet(rst, ns, collUuid);

// Execute different kinds of queries and check counters.
const cmdObj0 = {
    find: collName,
    filter: {x: 1},
};
const state0 = runCommandAndAssertCurrentOpAndServerStatus(
    opKindRead, cmdObj0, makeInitialCurrentOpAndServerStatusMongod(1));

const cmdObj1 = {
    count: collName,
};
const state1 = runCommandAndAssertCurrentOpAndServerStatus(opKindRead, cmdObj1, state0);

const cmdObj2 = {
    update: collName,
    updates: [{q: {x: 1}, u: {updated: true}}],
};
const state2 = runCommandAndAssertCurrentOpAndServerStatus(opKindWrite, cmdObj2, state1);

const cmdObj3 = {
    findAndModify: collName,
    query: {updated: true},
    update: {$set: {modified: 1}}
};
const state3 = runCommandAndAssertCurrentOpAndServerStatus(opKindWrite, cmdObj3, state2);

const cmdObj4 = {
    delete: collName,
    deletes: [{q: {x: 1}, limit: 1}],
};
const state4 = runCommandAndAssertCurrentOpAndServerStatus(opKindWrite, cmdObj4, state3);

// Stop query sampling.
assert.commandWorked(primary.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
QuerySamplingUtil.waitForInactiveSamplingReplicaSet(rst, ns, collUuid);

const expectedFinalState = Object.assign({}, state4, true /* deep */);
expectedFinalState.currentOp = [];
expectedFinalState.serverStatus.activeCollections = 0;

const actualFinalState = getCurrentOpAndServerStatusMongod(primary);
assert.eq(
    0, bsonWoCompare(actualFinalState, expectedFinalState), {actualFinalState, expectedFinalState});

rst.stopSet();
})();
