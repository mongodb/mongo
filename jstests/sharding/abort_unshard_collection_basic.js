/**
 * Tests for basic functionality of the abort unshard collection feature.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection
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

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

let failpoint = configureFailPoint(st.rs1.getPrimary(), 'reshardingPauseRecipientBeforeCloning');

// Starting the parallel shell for unshardCollectionCmd
const awaitResult = startParallelShell(
    funWithArgs(function(ns, toShardId) {
        assert.commandFailedWithCode(db.adminCommand({unshardCollection: ns, toShard: toShardId}),
                                     ErrorCodes.ReshardCollectionAborted);
    }, ns, st.shard1.shardName), st.s.port);

// Waiting to reach failpoint
failpoint.wait();

// Calling abortUnshardCollection
assert.commandWorked(mongos.adminCommand({abortUnshardCollection: ns}));

// Waiting for parallel shell to be finished
failpoint.off();
awaitResult();

st.stop();
})();
