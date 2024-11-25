/**
 * Tests that sharding state is properly initialized on config servers that undergo startup
 * recovery.
 *
 * @tags: [
 *   requires_persistence,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getLatestOp} from "jstests/replsets/rslib.js";
import {ShardingStateTest} from "jstests/sharding/libs/sharding_state_test.js";

const st = new ShardingTest({
    config: 1,
    shards: {rs0: {nodes: 1}},
    useHostname: false,
});
const configRS = st.configRS;
let primary = configRS.getPrimary();

jsTestLog("Adding shards so we can force startup recovery on the config primary");
const rs1 = new ReplSetTest({name: "newShard1", host: 'localhost', nodes: 1});
rs1.startSet({shardsvr: ""});
rs1.initiate();

assert.commandWorked(st.s.adminCommand({addShard: rs1.getURL(), name: "newShard1"}));

const ts = getLatestOp(primary).ts;
configureFailPoint(primary, 'holdStableTimestampAtSpecificTimestamp', {timestamp: ts});

const rs2 = new ReplSetTest({name: "newShard2", host: 'localhost', nodes: 1});
rs2.startSet({shardsvr: ""});
rs2.initiate();

assert.commandWorked(st.s.adminCommand({addShard: rs2.getURL(), name: "newShard2"}));

jsTestLog("Restarting node. It should go into startup recovery.");
configRS.restart(primary);
configRS.waitForState(primary, ReplSetTest.State.PRIMARY);

jsTestLog("Ensuring node is up as a primary and checking sharding state");
ShardingStateTest.failoverToMember(configRS, primary);
ShardingStateTest.checkShardingState(st);

jsTestLog("Adding another shard to check state");

const rs3 = new ReplSetTest({name: "newShard3", host: 'localhost', nodes: 1});
rs3.startSet({shardsvr: ""});
rs3.initiate();

assert.commandWorked(st.s.adminCommand({addShard: rs3.getURL(), name: "newShard3"}));

const shards = primary.getDB("config").getCollection("shards").find().toArray();
assert.eq(4, shards.length, () => tojson(shards));

jsTestLog("Done with test");

st.stop();
rs1.stopSet();
rs2.stopSet();
rs3.stopSet();
