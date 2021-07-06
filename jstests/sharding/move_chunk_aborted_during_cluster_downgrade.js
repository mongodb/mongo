/**
 * Launch a chunk migration while the cluster is downgrading FCV.
 * The chunk migration is expected to fail when the donor attempts to commit
 * the updated chunk metadata on the config server.
 *
 * TODO SERVER-53283: remove this test
 * once the migration protocol is allowed to run in the middle of an FCV operation again.
 *
 * @tags: [
 *   multiversion_incompatible,
 *  ]
 */
(function() {

"use strict";
load('./jstests/libs/chunk_manipulation_util.js');
load("jstests/libs/fail_point_util.js");

// 1. Setup a collection with a single chunk in a sharded cluster.
let st = new ShardingTest({shards: 2, mongos: 1});

const router = st.s0;
const dbName = 'TestDB';
const collName = `${dbName}.TestColl`;
const coll = router.getDB(dbName)[collName];
const donor = st.shard0;
const recipient = st.shard1;

assert.commandWorked(router.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, donor.shardName);
assert.commandWorked(router.adminCommand({shardCollection: collName, key: {Key: 1}}));
assert.commandWorked(coll.insert({Key: 0, Value: 'Value'}));

// 2. Launch a cluster downgrade operation and force a failure
// to reach a stable 'FCV kDowngrading' state.
const commandAbortedByTestFailpoint = 549181;
let abortFCVFailpoint = configureFailPoint(donor, 'failDowngrading');
assert.commandFailedWithCode(router.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             commandAbortedByTestFailpoint);

// 3. Verify that the chunk migration cannot be completed.
const expectedFailureMessage =
    'Cannot commit a chunk migration request while the cluster is being upgraded or downgraded';
let outcome = assert.commandFailedWithCode(
    router.adminCommand({moveChunk: collName, find: {Key: 0}, to: recipient.shardName}),
    ErrorCodes.ConflictingOperationInProgress);
assert.eq(expectedFailureMessage,
          outcome.errmsg,
          `Unexpected error message for moveChunk outcome ${outcome.errmsg}`);

// 4. Test teardown.
abortFCVFailpoint.off();
st.stop();
})();
