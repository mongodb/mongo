/**
 * Test to ensure that fcv change and add shard are mutually exclusive
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

jsTest.log("Running setFcv during an add shard should fail");

const cluster = new ShardingTest({shards: 1});

const rs = new ReplSetTest({nodes: 1});
rs.startSet({shardsvr: ""});
rs.initiate();

const addShardFP = configureFailPoint(cluster.configRS.getPrimary(),
                                      "hangAddShardBeforeUpdatingClusterCardinalityParameter");
const addShardParallelShell =
    startParallelShell(funWithArgs(function(url) {
                           assert.commandWorked(db.adminCommand({addShard: url, name: "newShard"}));
                       }, rs.getURL()), cluster.s.port);
addShardFP.wait();

assert.commandFailedWithCode(
    cluster.admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.ConflictingOperationInProgress);

addShardFP.off();
addShardParallelShell();

removeShard(cluster, "newShard");

jsTest.log("Running addShard during an fcv change should fail");

const setFcvFP = configureFailPoint(cluster.configRS.getPrimary(),
                                    "hangDowngradingBeforeIsCleaningServerMetadata");
const setFcvParallelShell = startParallelShell(
    funWithArgs(function(fcv) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));
    }, lastLTSFCV), cluster.s.port);
setFcvFP.wait();

assert.commandFailedWithCode(cluster.admin.runCommand({addShard: rs.getURL()}),
                             ErrorCodes.ConflictingOperationInProgress);

setFcvFP.off();
setFcvParallelShell();

rs.stopSet();
cluster.stop();
