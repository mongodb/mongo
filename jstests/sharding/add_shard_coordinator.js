/**
 * Tests the addShard coordinator while it is in development.
 *
 * TODO (SERVER-99284): remove this test once the new coordinator is used in the addShard
 * command.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

jsTest.log("Adding an already existing shard should return OK");
assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {_configsvrAddShardCoordinator: st.shard1.host, "writeConcern": {"w": "majority"}}));

jsTest.log("Adding an already existing shard with the existing name should return OK");
assert.commandWorked(st.configRS.getPrimary().adminCommand({
    _configsvrAddShardCoordinator: st.shard1.host,
    name: st.shard1.shardName,
    "writeConcern": {"w": "majority"}
}));

jsTest.log("Adding an already existing shard with different name should fail");
assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
    _configsvrAddShardCoordinator: st.shard1.host,
    name: "wolfee_is_smart",
    "writeConcern": {"w": "majority"}
}),
                             ErrorCodes.IllegalOperation);

jsTest.log("Adding a new shard should fail");
assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
    _configsvrAddShardCoordinator: "wolfee_is_smart",
    name: "200_IQ",
    "writeConcern": {"w": "majority"}
}),
                             ErrorCodes.NotImplemented);

jsTest.log("Empty name should fail");
assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
    _configsvrAddShardCoordinator: st.shard1.host,
    name: "",
    "writeConcern": {"w": "majority"}
}),
                             ErrorCodes.BadValue);

st.stop();
