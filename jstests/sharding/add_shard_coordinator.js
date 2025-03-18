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
 *   requires_persistence,
 *   multiversion_incompatible,
 *   featureFlagUseTopologyChangeCoordinators,
 * ]
 */

// TODO(SERVER-96770): this test should not exist after enabling the
// featureFlagUseTopologyChangeCoordinators - the functionality should be tested through the public
// API

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const clusterParameter1Value = {
    intData: 42
};
const clusterParameter1Name = 'testIntClusterParameter';
const clusterParameter1 = {
    [clusterParameter1Name]: clusterParameter1Value
};

const clusterParameter2Value = {
    strData: 'on'
};
const clusterParameter2Name = 'testStrClusterParameter';
const clusterParameter2 = {
    [clusterParameter2Name]: clusterParameter2Value
};

const clusterParameter3Value = {
    boolData: true
};
const clusterParameter3_4Name = 'testBoolClusterParameter';
const clusterParameter3 = {
    [clusterParameter3_4Name]: clusterParameter3Value
};

const clusterParameter4Value = {
    boolData: false
};
const clusterParameter4 = {
    [clusterParameter3_4Name]: clusterParameter4Value
};

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
                                 ErrorCodes.OperationFailed);

    st.stop();
}

{
    const st = new ShardingTest({shards: 0});
    const rs = new ReplSetTest({nodes: 1});
    rs.startSet();
    rs.initiate();

    assert.commandWorked(rs.getPrimary().adminCommand({setClusterParameter: clusterParameter1}));

    rs.restart(0, {shardsvr: ""});

    const foo = rs.getPrimary().getDB("foo");
    assert.commandWorked(foo.foo.insertOne({a: 1}));

    jsTest.log("Non-empty RS can be added if this is the first shard");
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrAddShardCoordinator: rs.getURL(), "writeConcern": {"w": "majority"}}));

    jsTest.log("First RS is never locked for write");
    assert.commandWorked(foo.foo.insertOne({b: 1}));

    jsTest.log("Cluster parameters synchronized correctly");
    const shardParametersConfigColl = rs.getPrimary().getCollection('config.clusterParameters');
    const clusterParametersConfigColl =
        st.configRS.getPrimary().getCollection('config.clusterParameters');
    assert.eq(1, shardParametersConfigColl.countDocuments({_id: clusterParameter1Name}));
    assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameter1Name}));

    rs.stopSet();
    st.stop();
}

{
    const st = new ShardingTest({shards: 1});

    assert.commandWorked(st.s.adminCommand({setClusterParameter: clusterParameter1}));
    assert.commandWorked(st.s.adminCommand({setClusterParameter: clusterParameter3}));

    const rs = new ReplSetTest({nodes: 1, name: "rs"});
    rs.startSet();
    rs.initiate();

    assert.commandWorked(rs.getPrimary().adminCommand({setClusterParameter: clusterParameter2}));
    assert.commandWorked(rs.getPrimary().adminCommand({setClusterParameter: clusterParameter4}));
    assert.commandWorked(
        rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    rs.restart(0, {shardsvr: ""});

    assert.neq(rs.getPrimary().getDB("admin").system.version.findOne().version,
               st.configRS.getPrimary().getDB("admin").system.version.findOne().version);

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
    {
        const response = st.configRS.getPrimary().adminCommand({
            _configsvrAddShardCoordinator: rs.getURL(),
            name: "hagymasbab",
            "writeConcern": {"w": "majority"}
        });
        assert.commandWorked(response);
        assert.eq(response.shardAdded, "hagymasbab");
    }
    {
        const response = st.s.adminCommand({listShards: 1});
        assert.commandWorked(response);
        assert.eq(response.shards.length, 2);
        assert(response.shards.some(shard => shard._id === "hagymasbab"));
    }

    jsTest.log("Non-first RS is unlocked after write");
    assert.commandWorked(rs.getPrimary().getDB("foo").foo.insertOne({b: 1}));

    jsTest.log("Shard identity must be valid");
    const shardIdentityDoc =
        rs.getPrimary().getDB("admin").system.version.findOne({_id: "shardIdentity"});
    assert.neq(shardIdentityDoc, null);

    jsTest.log("Shard identity must be valid");
    assert.eq(shardIdentityDoc.shardName, "hagymasbab");

    jsTest.log("Cluster parameters synchronized correctly");
    const shardParametersConfigColl = rs.getPrimary().getCollection('config.clusterParameters');
    const clusterParametersConfigColl =
        st.configRS.getPrimary().getCollection('config.clusterParameters');
    assert.eq(1, shardParametersConfigColl.countDocuments({_id: clusterParameter1Name}));
    assert.eq(0, shardParametersConfigColl.countDocuments({_id: clusterParameter2Name}));
    assert.eq(1, shardParametersConfigColl.countDocuments({_id: clusterParameter3_4Name}));

    assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameter1Name}));
    assert.eq(0, clusterParametersConfigColl.countDocuments({_id: clusterParameter2Name}));
    assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameter3_4Name}));

    assert.eq(shardParametersConfigColl.findOne({_id: clusterParameter3_4Name},
                                                {_id: 0, clusterParameterTime: 0}),
              clusterParametersConfigColl.findOne({_id: clusterParameter3_4Name},
                                                  {_id: 0, clusterParameterTime: 0}));

    jsTest.log("FCV must match");
    assert.neq(rs.getPrimary().getDB("admin").system.version.findOne().version,
               st.configRS.getPrimary().getDB("admin").system.version.findOne());

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    const rs2 = new ReplSetTest({nodes: 1, name: "rs2"});
    rs2.startSet({shardsvr: ""});
    rs2.initiate();

    {
        const response = st.configRS.getPrimary().adminCommand({
            _configsvrAddShardCoordinator: rs2.getURL(),
            name: "krumplishal",
            "writeConcern": {"w": "majority"}
        });
        assert.commandWorked(response);
        assert.eq(response.shardAdded, "krumplishal");
    }

    jsTest.log("Cluster-wide user write blocking is propagated");
    try {
        rs2.getPrimary().getDB("bar").foo.insertOne({b: 1});
    } catch (error) {
        assert.commandFailedWithCode(error, ErrorCodes.UserWritesBlocked);
    }

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));

    assert.commandWorked(rs2.getPrimary().getDB("bar").foo.insertOne({b: 1}));

    rs.stopSet();
    st.stop();
    rs2.stopSet();
}
