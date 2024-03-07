/**
 * Tests that resharding ops abort take provenance into account.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 1});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

let failpoint = configureFailPoint(st.rs0.getPrimary(), 'reshardingPauseRecipientDuringCloning');

const awaitResult = startParallelShell(
    funWithArgs(function(ns) {
        assert.commandFailedWithCode(db.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                                     ErrorCodes.ReshardCollectionAborted);
    }, ns), st.s.port);

failpoint.wait();

const filter = {
    type: "op",
    "originatingCommand.reshardCollection": ns,
    "provenance": "reshardCollection"
};
assert.soon(() => {
    return st.s.getDB("admin")
               .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
               .toArray()
               .length >= 1;
});

assert.commandFailedWithCode(mongos.adminCommand({abortUnshardCollection: ns}),
                             ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(mongos.adminCommand({abortMoveCollection: ns}),
                             ErrorCodes.IllegalOperation);

assert.commandWorked(mongos.adminCommand({abortReshardCollection: ns}));

failpoint.off();
awaitResult();

const metrics = st.config0.getDB('admin').serverStatus({}).shardingStatistics.resharding;

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSucceeded, 0);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 1);

st.stop();
})();
