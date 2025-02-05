/**
 * Tests the addShard coordinator while it is in development.
 *
 * TODO (SERVER-99284): remove this test once the new coordinator is used in the addShard
 * command.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 *   config_shard_incompatible,
 * ]
 */

// TODO(SERVER-96770): this test should not exist after enabling the
// featureFlagUseTopologyChangeCoordinators - the functionality should be tested through the public
// API

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

{
    const st = new ShardingTest({shards: 2, nodes: 1});

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

    jsTest.log("Empty name should fail");
    assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
        _configsvrAddShardCoordinator: st.shard1.host,
        name: "",
        "writeConcern": {"w": "majority"}
    }),
                                 ErrorCodes.BadValue);

    jsTest.log("Invalid host should fail");
    assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
        _configsvrAddShardCoordinator: "some.nonexistent.host",
        "writeConcern": {"w": "majority"}
    }),
                                 ErrorCodes.HostUnreachable);

    st.stop();
}

{
    const st = new ShardingTest({shards: 0});
    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();

    const foo = rs.getPrimary().getDB("foo");
    assert.commandWorked(foo.foo.insertOne({a: 1}));

    jsTest.log("Non-empty RS can be added if this is the first shard");
    assert.commandFailedWithCode(
        st.configRS.getPrimary().adminCommand(
            {_configsvrAddShardCoordinator: rs.getURL(), "writeConcern": {"w": "majority"}}),
        ErrorCodes.NotImplemented);

    jsTest.log("First RS is never locked for write");
    assert.commandWorked(foo.foo.insertOne({b: 1}));

    rs.stopSet();
    st.stop();
}

{
    const st = new ShardingTest({shards: 1});
    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();

    assert.commandWorked(rs.getPrimary().getDB("foo").foo.insertOne({a: 1}));

    jsTest.log("Non-empty RS can't be added if it's not the first shard");
    assert.commandFailedWithCode(
        st.configRS.getPrimary().adminCommand(
            {_configsvrAddShardCoordinator: rs.getURL(), "writeConcern": {"w": "majority"}}),
        ErrorCodes.IllegalOperation);

    jsTest.log("RS user writes is unlocked on fail");
    assert.commandWorked(rs.getPrimary().getDB("foo").foo.insertOne({b: 1}));
    assert.commandWorked(rs.getPrimary().getDB("foo").dropDatabase({w: "majority"}));

    jsTest.log("Empty non-first RS can be added");
    assert.commandFailedWithCode(
        st.configRS.getPrimary().adminCommand(
            {_configsvrAddShardCoordinator: rs.getURL(), "writeConcern": {"w": "majority"}}),
        ErrorCodes.NotImplemented);

    jsTest.log("Non-first RS is locked for write");
    try {
        rs.getPrimary().getDB("bar").foo.insertOne({b: 1});
    } catch (error) {
        assert.commandFailedWithCode(error, ErrorCodes.UserWritesBlocked);
    }

    rs.stopSet();
    st.stop();
}
