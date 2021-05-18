/**
 * Tests that a drop can't happen while resharding is in progress.
 *
 * @tags: [
 *   requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

// Ensures that resharding has acquired the db and collection distLocks. The fact that the entry in
// config.reshardingOperations exists guarantees that the distLocks have already been acquired.
function awaitReshardingStarted() {
    assert.soon(() => {
        const coordinatorDoc = st.s.getCollection("config.reshardingOperations").findOne({ns: ns});
        return coordinatorDoc !== null;
    }, "resharding didn't start");
}

var st = new ShardingTest({
    shards: 1,
    config: 1,
    mongos: 1,
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const db = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

const pauseCoordinatorBeforeDecisionPersistedFailpoint = configureFailPoint(
    st.configRS.getPrimary(), "reshardingPauseCoordinatorBeforeDecisionPersisted");

const awaitReshardResult = startParallelShell(
    funWithArgs(function(ns) {
        assert.commandWorked(db.adminCommand({reshardCollection: ns, key: {newKey: 1}}));
    }, ns), st.s.port);

awaitReshardingStarted();

let res = db.runCommand({drop: collName, maxTimeMS: 5000});
assert(ErrorCodes.isExceededTimeLimitError(res.code));

pauseCoordinatorBeforeDecisionPersistedFailpoint.off();
awaitReshardResult();

st.stop();
})();
