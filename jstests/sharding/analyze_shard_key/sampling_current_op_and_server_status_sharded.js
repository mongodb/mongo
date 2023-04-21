/**
 * Tests the currentOp and serverStatus for query sampling on a sharded cluster.
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

const st = new ShardingTest({
    shards: 1,
    mongos: [
        {
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs,
                logComponentVerbosity: tojson({sharding: 2})
            }
        },
        {
            setParameter: {
                // This failpoint will force this mongos to not count any queries that it runs.
                // This will force its local sample rate to be exactly 0.
                "failpoint.overwriteQueryAnalysisSamplerAvgLastCountToZero":
                    tojson({mode: "alwaysOn"}),
                queryAnalysisSamplerConfigurationRefreshSecs,
                logComponentVerbosity: tojson({sharding: 2})
            }
        }
    ],
    rs: {nodes: 1, setParameter: {logComponentVerbosity: tojson({sharding: 2})}}
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const numDocs = 10;
const sampleRate = 1000;

const db = st.s0.getDB(dbName);
const coll = db.getCollection(collName);
// Insert initial documents.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({x: i, y: i});
}
assert.commandWorked(bulk.execute());
const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

function makeInitialCurrentOpAndServerStatus(numColls) {
    return {
        mongos0: makeInitialCurrentOpAndServerStatusMongos(numColls),
        mongos1: makeInitialCurrentOpAndServerStatusMongos(numColls),
        mongod: makeInitialCurrentOpAndServerStatusMongod(numColls),
    };
}

function getCurrentOpAndServerStatus() {
    return {
        mongos0: getCurrentOpAndServerStatusMongos(st.s0),
        mongos1: getCurrentOpAndServerStatusMongos(st.s1),
        mongod: getCurrentOpAndServerStatusMongod(st.rs0.getPrimary())
    };
}

function runCommandAndAssertCurrentOpAndServerStatus(opKind, cmdObj, oldState) {
    // Only run commands against mongos0 since mongos1 is supposed to be inactive.
    assert.commandWorked(st.s0.getDB(dbName).runCommand(cmdObj));

    let newState;
    assert.soon(() => {
        newState = getCurrentOpAndServerStatus();
        return assertCurrentOpAndServerStatusMongos(
                   ns, opKind, oldState.mongos0, newState.mongos0) &&
            assertCurrentOpAndServerStatusMongos(
                   ns, opKindNoop, oldState.mongos1, newState.mongos1, {expectedSampleRate: 0}) &&
            assertCurrentOpAndServerStatusMongod(
                   ns, opKind, oldState.mongod, newState.mongod, true /* isShardSvr */);
    });
    return newState;
}

let currentState = getCurrentOpAndServerStatus();
assert.eq(bsonWoCompare(currentState, makeInitialCurrentOpAndServerStatus(0)), 0, {currentState});

// Start query sampling.
assert.commandWorked(
    st.s0.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: sampleRate}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collUuid);
// Wait for at least one refresh interval to make the inactive mongos find out that its sample rate
// is 0.
sleep(2 * queryAnalysisSamplerConfigurationRefreshSecs);

// Execute different kinds of queries and check counters.
const cmdObj0 = {
    find: collName,
    filter: {x: 1},
};
const state0 = runCommandAndAssertCurrentOpAndServerStatus(
    opKindRead, cmdObj0, makeInitialCurrentOpAndServerStatus(1));

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
assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
QuerySamplingUtil.waitForInactiveSamplingShardedCluster(st, ns, collUuid);

const expectedFinalState = Object.assign({}, state4, true /* deep */);
expectedFinalState.mongos0.currentOp = [];
expectedFinalState.mongos1.currentOp = [];
expectedFinalState.mongod.currentOp = [];
expectedFinalState.mongos0.serverStatus.activeCollections = 0;
expectedFinalState.mongos1.serverStatus.activeCollections = 0;
expectedFinalState.mongod.serverStatus.activeCollections = 0;

const actualFinalState = getCurrentOpAndServerStatus();
assert.eq(
    0, bsonWoCompare(actualFinalState, expectedFinalState), {actualFinalState, expectedFinalState});

st.stop();
})();
