/**
 * Test that a replica set with an existing but different shard identity document cannot be added to
 * a sharded cluster.
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from 'jstests/sharding/libs/remove_shard_util.js';

jsTest.log("Creating a sharded cluster with one shard");
const st0 = new ShardingTest({name: "st0", shards: 1});

jsTest.log("Creating a single node replica set");
const rs0 = new ReplSetTest({name: "rs0", nodes: 1});
rs0.startSet({shardsvr: ""});
rs0.initiate();

jsTest.log("Adding replica set to the sharded cluster");
assert.commandWorked(st0.s.adminCommand({addShard: rs0.getURL(), name: "testName"}));

jsTest.log("Removing just added shard from the sharded cluster");
removeShard(st0, "testName");

jsTest.log("Adding replica set to the sharded cluster with the same name (same shardIdentity)");
assert.commandWorked(st0.s.adminCommand({addShard: rs0.getURL(), name: "testName"}));

jsTest.log("Removing just added shard from the sharded cluster");
removeShard(st0, "testName");

jsTest.log(
    "Adding replica set to the sharded cluster with a different name (different shardIdentity)");
assert.commandFailedWithCode(st0.s.adminCommand({addShard: rs0.getURL(), name: "diffTestName"}),
                             ErrorCodes.IllegalOperation);

jsTest.log("Stopping sharded cluster");
st0.stop();

jsTest.log("Creating a new sharded cluster with one shard");
const st1 = new ShardingTest({name: "st1", shards: 1});

jsTest.log(
    "Adding replica set to a different sharded cluster with the same name (same shardIdentity)");
assert.commandFailedWithCode(st1.s.adminCommand({addShard: rs0.getURL(), name: "testName"}),
                             ErrorCodes.IllegalOperation);

jsTest.log("Stopping sharded cluster and replica set");
st1.stop();
rs0.stopSet();
