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

function awaitReshardingStarted() {
    assert.soon(() => {
        const op = st.admin
                       .aggregate([
                           {$currentOp: {allUsers: true, localOps: true}},
                           {$match: {"command.reshardCollection": ns}},
                       ])
                       .toArray()[0];

        return op !== undefined;
    }, "failed to find reshardCollection in $currentOp output");
}

var st = new ShardingTest({
    shards: 1,
    config: 1,
    mongos: 1,
    other: {
        mongosOptions: {setParameter: {featureFlagResharding: true}},
        configOptions: {
            setParameter: {
                featureFlagResharding: true,
                reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000
            }
        }
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

assert.commandFailedWithCode(db.runCommand({drop: collName, maxTimeMS: 5000}),
                             ErrorCodes.MaxTimeMSExpired);

pauseCoordinatorBeforeDecisionPersistedFailpoint.off();
awaitReshardResult();

st.stop();
})();
