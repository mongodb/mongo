/**
 * This test asserts that an illegal OpID passed to mongos' implementation of killOp results in a
 * failure being propagated back to the client.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({name: "shard1", shards: 1, mongos: 1});

assert.commandFailed(st.s.getDB("admin").runCommand(
    {killOp: 1, op: st.shard0.shardName + ":99999999999999999999999"}));
st.stop();