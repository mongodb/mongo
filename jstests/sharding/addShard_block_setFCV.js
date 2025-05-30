/**
 * Test that during an addShard setFeatureCompatibilityVersion commands run via direct connections
 * are blocked on the replica set being added to the cluster.
 *
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible,
 *   featureFlagUseTopologyChangeCoordinators
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

jsTest.log("Creating a sharded cluster with one shard");
var st = new ShardingTest({name: "st", shards: 1});

jsTest.log("Creating a single node replica set");
var rs0 = new ReplSetTest({name: "rs0", nodes: 1});
rs0.startSet({shardsvr: ""});
rs0.initiate();

jsTest.log("Checking that RS is not locked for setting the FCV before running addShard");
let adminDB = rs0.getPrimary().getDB("admin");
assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

jsTest.log("Run an addShard command but pause immediately after blocking setFCV commands");
const configPrimary = st.configRS.getPrimary();
const addShardFp = configureFailPoint(configPrimary, "hangAfterLockingNewShard");
const awaitResult = startParallelShell(funWithArgs(function(url) {
                                           assert.commandWorked(db.adminCommand({addShard: url}));
                                       }, rs0.getURL()), st.s.port);
addShardFp.wait();

jsTest.log("Checking that RS is locked for setting the FCV after running addShard");
assert.commandFailedWithCode(
    adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.ConflictingOperationInProgress);

// Release fail point
addShardFp.off();
awaitResult();

// Stop sharded cluster and replica set
st.stop();
rs0.stopSet();
