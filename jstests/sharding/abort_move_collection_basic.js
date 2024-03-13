/**
 * Tests for basic functionality of the abort move collection feature.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));

// TODO (SERVER-86295) Replace createUnsplittableCollection with create once moveCollection
// registers the collection on the sharding catalog
assert.commandWorked(st.s.getDB(dbName).runCommand({createUnsplittableCollection: collName}));

const coll = mongos.getDB(dbName)[collName];
for (let i = -5; i < 5; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}

let failpoint = configureFailPoint(st.rs1.getPrimary(), 'reshardingPauseRecipientDuringCloning');

// Starting the parallel shell for moveCollectionCmd
const awaitResult = startParallelShell(
    funWithArgs(function(ns, toShardId) {
        assert.commandFailedWithCode(db.adminCommand({moveCollection: ns, toShard: toShardId}),
                                     ErrorCodes.ReshardCollectionAborted);
    }, ns, shard1), st.s.port);

// Waiting to reach failpoint
failpoint.wait();

// Verify that the provenance field is appended to the currentOp
const filter = {
    type: "op",
    "originatingCommand.reshardCollection": ns,
    "provenance": "moveCollection"
};
assert.soon(() => {
    return st.s.getDB("admin")
               .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
               .toArray()
               .length >= 1;
});

// Calling abortMoveCollection
assert.commandWorked(mongos.adminCommand({abortMoveCollection: ns}));

// Waiting for parallel shell to be finished
failpoint.off();
awaitResult();

const metrics = st.config0.getDB('admin').serverStatus({}).shardingStatistics.moveCollection;

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSucceeded, 0);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 1);

assert.eq(0, st.rs1.getPrimary().getCollection(ns).countDocuments({}));
assert.eq(10, st.rs0.getPrimary().getCollection(ns).countDocuments({}));

st.stop();
})();
