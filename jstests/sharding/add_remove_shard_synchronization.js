/**
 * Tests only one add or remove shard can be executed at once.
 * @tags: [
 *    featureFlagUseTopologyChangeCoordinators,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
});

const rs = new ReplSetTest({name: "extraShard", nodes: 1, nodeOptions: {shardsvr: ""}});
rs.startSet();
rs.initiate();

// Configure a shorter DDL lock timeout since we test lock timeouts in this test.
const shorterLockTimeout = configureFailPoint(
    st.configRS.getPrimary(), "overrideDDLLockTimeout", {'timeoutMillisecs': 500});

jsTest.log("Test that only one add shard can run at a given time.");

// Pause the addShardCoordinator so that we can check that another topology change will fail.
let addShardFP = configureFailPoint(st.configRS.getPrimary(),
                                    "hangAddShardBeforeUpdatingClusterCardinalityParameter");

// Issue the async add shard and wait for it to hit the failpoint.
const awaitAddShard =
    startParallelShell(funWithArgs(function(url, name) {
                           assert.commandWorked(db.adminCommand({addShard: url, name: name}));
                       }, rs.getURL(), "extraShard"), st.s.port);
jsTest.log("Waiting for failpoint");
addShardFP.wait();
jsTest.log("Hit failpoint");

// Add shard with different options should throw.
assert.commandFailedWithCode(st.s.adminCommand({addShard: rs.getURL(), name: "differentName"}),
                             ErrorCodes.ConflictingOperationInProgress);
assert.commandFailedWithCode(st.s.adminCommand({addShard: "fakeShardUrl"}),
                             ErrorCodes.ConflictingOperationInProgress);

// Remove shard will time out waiting for the DDL lock.
assert.commandFailedWithCode(
    st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}),
    ErrorCodes.LockBusy);

jsTest.log("Let the add shard complete");
addShardFP.off();
awaitAddShard();

jsTest.log("Test that only one remove shard can run at a given time.");

// Pause the removeShardCommitCoordinator so that we can check that another topology change will
// fail.
let removeShardFailpoint = configureFailPoint(
    st.configRS.getPrimary(), "hangRemoveShardBeforeUpdatingClusterCardinalityParameter");

// Start the shard draining.
assert.commandWorked(st.s.adminCommand({removeShard: rs.getURL()}));
// Issue the async remove shard and wait for it to hit the failpoint.
const awaitRemoveShard = startParallelShell(
    funWithArgs(function(shard) {
        assert.commandWorked(
            db.adminCommand({_configsvrRemoveShardCommit: shard, writeConcern: {w: "majority"}}));
    }, rs.getURL()), st.configRS.getPrimary().port);
removeShardFailpoint.wait();

// RemoveShard of a different shard should throw.
assert.commandFailedWithCode(
    st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}),
    ErrorCodes.ConflictingOperationInProgress);

// Add shard will time out waiting for the DDL lock.
assert.commandFailedWithCode(st.s.adminCommand({addShard: st.rs0.getURL()}), ErrorCodes.LockBusy);

jsTest.log("Let the remove shard complete");
removeShardFailpoint.off();
awaitRemoveShard();

shorterLockTimeout.off();
st.stop();
rs.stopSet();
