/**
 * Checks that setClusterParameter command only run once
 *
 * We have a restart in the test with some stored values that must be preserved so it cannot run in
 * inMemory variants
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// This test requires running commands directly against the shard.
TestData.replicaSetEndpointIncompatible = true;

const clusterParameter1Value = {
    intData: 42,
};
const clusterParameter1Name = "testIntClusterParameter";
const clusterParameter1 = {
    [clusterParameter1Name]: clusterParameter1Value,
};

const clusterParameter2Value = {
    strData: "on",
};
const clusterParameter2Name = "testStrClusterParameter";
const clusterParameter2 = {
    [clusterParameter2Name]: clusterParameter2Value,
};

const clusterParameter3Value = {
    boolData: true,
};
const clusterParameter3Name = "testBoolClusterParameter";
const clusterParameter3 = {
    [clusterParameter3Name]: clusterParameter3Value,
};

const checkClusterParameters = (clusterParameterName, clusterParameterValue, configRS, shard, checkTime = true) => {
    const shardParametersConfigColl = shard.getCollection("config.clusterParameters");
    const clusterParametersConfigColl = configRS.getCollection("config.clusterParameters");

    assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameterName}));
    const configClusterParameter = clusterParametersConfigColl.findOne(
        {_id: clusterParameterName},
        {_id: 0, clusterParameterTime: 0},
    );
    const shardClusterParameter = shardParametersConfigColl.findOne(
        {_id: clusterParameterName},
        {_id: 0, clusterParameterTime: 0},
    );
    assert.docEq(configClusterParameter, clusterParameterValue);
    assert.docEq(shardClusterParameter, clusterParameterValue);

    if (checkTime) {
        // Check the full cluster has the same clusterParameterTime as the config server.
        const configParameterTime = clusterParametersConfigColl.findOne(
            {_id: clusterParameterName},
            {clusterParameterTime: 1},
        ).clusterParameterTime;
        assert.eq(
            configParameterTime,
            shardParametersConfigColl.findOne({_id: clusterParameterName}, {clusterParameterTime: 1})
                .clusterParameterTime,
        );
    }
};

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 3}});

    {
        jsTestLog(
            "Check that 2 requests for the same cluster parameter and same value generates only one coordinator.",
        );

        let fp = configureFailPoint(st.configRS.getPrimary(), "hangBeforeRunningConfigsvrCoordinatorInstance");

        let setClusterParameterSuccessThread = new Thread(
            (mongosConnString, clusterParameter) => {
                let mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
            },
            st.s.host,
            clusterParameter1,
        );

        setClusterParameterSuccessThread.start();
        fp.wait();

        let setClusterParameterJoinSuccessThread = new Thread(
            (mongosConnString, clusterParameter) => {
                let mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
            },
            st.s.host,
            clusterParameter1,
        );

        setClusterParameterJoinSuccessThread.start();
        fp.wait();

        let currOp = st.configRS
            .getPrimary()
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true}}, {$match: {desc: "SetClusterParameterCoordinator"}}])
            .toArray();
        assert.eq(1, currOp.length);
        assert(currOp[0].hasOwnProperty("command"));
        assert.docEq(currOp[0].command, clusterParameter1);

        jsTestLog("Check that a second request will fail with ConflictingOperationInProgress.");

        assert.commandFailedWithCode(
            st.s.adminCommand({setClusterParameter: clusterParameter2}),
            ErrorCodes.ConflictingOperationInProgress,
        );

        fp.off();
        setClusterParameterSuccessThread.join();
        setClusterParameterJoinSuccessThread.join();
    }

    {
        jsTestLog("Check forward progress until completion in the presence of a config server stepdown.");

        let fp = configureFailPoint(st.configRS.getPrimary(), "hangBeforeRunningConfigsvrCoordinatorInstance");

        let setClusterParameterThread = new Thread(
            (mongosConnString, clusterParameter) => {
                let mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
            },
            st.s.host,
            clusterParameter2,
        );

        setClusterParameterThread.start();
        fp.wait();

        let newPrimary = st.configRS.getSecondary();

        st.configRS.stepUp(newPrimary);

        // After the stepdown the command should be retried and finish successfully.
        setClusterParameterThread.join();

        checkClusterParameters(
            clusterParameter2Name,
            clusterParameter2Value,
            st.configRS.getPrimary(),
            st.rs0.getPrimary(),
        );

        fp.off();
    }

    st.stop();
}

if (!TestData.configShard) {
    {
        const st2 = new ShardingTest({mongos: 1, shards: 0, name: "second_cluster"});

        const newShard2Name = "newShard2";
        const newShard2 = new ReplSetTest({name: newShard2Name, nodes: 1});
        newShard2.startSet();
        newShard2.initiate();

        {
            jsTestLog(
                "Check that on replica set to sharded cluster transition parameters added in RS are synced in the cluster.",
            );
            assert.commandWorked(newShard2.getPrimary().adminCommand({setClusterParameter: clusterParameter1}));

            newShard2.restart(0, {shardsvr: ""});
            newShard2.awaitNodesAgreeOnPrimary();

            // After restarting a node with --shardsvr it should not accept setClusterParameter
            // commands.
            assert.commandFailedWithCode(
                newShard2.getPrimary().adminCommand({setClusterParameter: clusterParameter3}),
                ErrorCodes.NotImplemented,
            );

            checkClusterParameters(
                clusterParameter1Name,
                clusterParameter1Value,
                newShard2.getPrimary(),
                newShard2.getPrimary(),
                false,
            );

            assert.commandWorked(st2.s.adminCommand({addShard: newShard2.getURL(), name: newShard2Name}));

            // After adding the shard there the clusterParameter set on the rs clusterParameter1
            // must have been adopted by the whole cluster.
            checkClusterParameters(
                clusterParameter1Name,
                clusterParameter1Value,
                st2.configRS.getPrimary(),
                newShard2.getPrimary(),
            );

            assert.eq(
                st2.configRS.getPrimary().getCollection("config.clusterParameters").countDocuments({}),
                newShard2.getPrimary().getCollection("config.clusterParameters").countDocuments({}),
            );
        }

        {
            jsTestLog("Check that parameters added in cluster overwrite custom RS parameters.");

            const newShard3Name = "newShard3";
            const newShard3 = new ReplSetTest({name: newShard3Name, nodes: 1});
            newShard3.startSet();
            newShard3.initiate();

            assert.commandWorked(newShard3.getPrimary().adminCommand({setClusterParameter: clusterParameter2}));

            newShard3.restart(0, {shardsvr: ""});
            newShard3.awaitNodesAgreeOnPrimary();

            checkClusterParameters(
                clusterParameter2Name,
                clusterParameter2Value,
                newShard3.getPrimary(),
                newShard3.getPrimary(),
            );

            assert.commandWorked(st2.s.adminCommand({addShard: newShard3.getURL(), name: newShard3Name}));

            // After adding the shard there must be only one cluster parameter: the one set on the
            // cluster clusterParameter1.
            checkClusterParameters(
                clusterParameter1Name,
                clusterParameter1Value,
                st2.configRS.getPrimary(),
                newShard3.getPrimary(),
            );
            assert.eq(
                0,
                newShard3.getPrimary().getCollection("config.clusterParameters").countDocuments({
                    _id: clusterParameter2Name,
                }),
            );

            // Well behaved test, remove shard and stop the set.
            removeShard(st2, newShard3Name);

            newShard3.stopSet();
        }

        newShard2.stopSet();

        st2.stop();
    }
    {
        jsTestLog("Check that parameters added in an empty cluster overwrite custom RS parameters.");

        const st3 = new ShardingTest({mongos: 1, shards: 0, name: "third_cluster"});

        st3.s.adminCommand({setClusterParameter: clusterParameter1});

        const newShard4Name = "newShard2";
        const newShard4 = new ReplSetTest({name: newShard4Name, nodes: 1});
        newShard4.startSet();
        newShard4.initiate();

        newShard4.getPrimary().adminCommand({setClusterParameter: clusterParameter2});

        newShard4.restart(0, {shardsvr: ""});
        newShard4.awaitNodesAgreeOnPrimary();

        checkClusterParameters(
            clusterParameter2Name,
            clusterParameter2Value,
            newShard4.getPrimary(),
            newShard4.getPrimary(),
        );

        assert.commandWorked(st3.s.adminCommand({addShard: newShard4.getURL(), name: newShard4Name}));

        checkClusterParameters(
            clusterParameter1Name,
            clusterParameter1Value,
            st3.configRS.getPrimary(),
            newShard4.getPrimary(),
        );

        newShard4.stopSet();

        st3.stop();
    }

    {
        jsTestLog("Check that addShard serializes with setClusterParameter.");

        const st4 = new ShardingTest({shards: 1, rs: {nodes: 3}});

        const newShardName = "newShard";
        const newShard = new ReplSetTest({name: newShardName, nodes: 1});
        newShard.startSet({shardsvr: ""});
        newShard.initiate();

        let shardsvrSetClusterParameterFailPoint = configureFailPoint(
            st4.rs0.getPrimary(),
            "hangInShardsvrSetClusterParameter",
        );

        let setClusterParameterThread = new Thread(
            (mongosConnString, clusterParameter) => {
                let mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
            },
            st4.s.host,
            clusterParameter3,
        );
        setClusterParameterThread.start();

        shardsvrSetClusterParameterFailPoint.wait();

        assert.commandFailedWithCode(
            st4.s.adminCommand({addShard: newShard.getURL(), name: newShardName, maxTimeMS: 1000}),
            ErrorCodes.MaxTimeMSExpired,
        );

        shardsvrSetClusterParameterFailPoint.off();
        setClusterParameterThread.join();

        jsTestLog("Check that the config server will push all parameters when adding a new shard.");

        assert.commandWorked(st4.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));

        checkClusterParameters(
            clusterParameter3Name,
            clusterParameter3Value,
            st4.configRS.getPrimary(),
            newShard.getPrimary(),
        );

        removeShard(st4, newShardName);

        newShard.stopSet();

        // Final check, the initial shard has all the cluster parameters
        checkClusterParameters(
            clusterParameter3Name,
            clusterParameter3Value,
            st4.configRS.getPrimary(),
            st4.rs0.getPrimary(),
        );

        st4.stop();
    }
} else {
    // In config shard mode
    {
        jsTestLog("Check that RS which transitions to a config shard keeps cluster params.");

        const configShardName = "configShard";
        const configShard = new ReplSetTest({
            name: configShardName,
            nodes: 1,
            nodeOptions: {setParameter: {skipShardingConfigurationChecks: true}},
        });
        configShard.startSet();
        configShard.initiate();

        configShard.getPrimary().adminCommand({setClusterParameter: clusterParameter2});

        let cfg = configShard.getReplSetConfigFromNode();
        cfg.configsvr = true;
        reconfig(configShard, cfg);

        configShard.restart(0, {
            configsvr: "",
            setParameter: {skipShardingConfigurationChecks: false, featureFlagTransitionToCatalogShard: true},
        });
        configShard.awaitNodesAgreeOnPrimary();

        // Cluster params should still exist.
        checkClusterParameters(
            clusterParameter2Name,
            clusterParameter2Value,
            configShard.getPrimary(),
            configShard.getPrimary(),
        );

        if (TestData.mongosBinVersion) {
            // Lower the config shard's FCV so an earlier binary mongos can connect.
            const targetFCV = binVersionToFCV(TestData.mongosBinVersion);
            assert.commandWorked(
                configShard.getPrimary().adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
            );
        }
        let mongos = MongoRunner.runMongos({
            configdb: configShard.getURL(),
            setParameter: "featureFlagTransitionToCatalogShard=true",
        });
        assert.commandWorked(mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));
        checkClusterParameters(
            clusterParameter2Name,
            clusterParameter2Value,
            configShard.getPrimary(),
            configShard.getPrimary(),
        );

        // Config shard should not accept cluster parameters set directly on it.
        assert.commandFailedWithCode(
            configShard.getPrimary().adminCommand({setClusterParameter: clusterParameter3}),
            ErrorCodes.NotImplemented,
        );

        jsTestLog("Check that parameters added in a config shard cluster overwrite custom RS parameters.");

        const newShard5Name = "newShard5";
        const newShard5 = new ReplSetTest({name: newShard5Name, nodes: 1});
        newShard5.startSet();
        newShard5.initiate();

        newShard5.getPrimary().adminCommand({setClusterParameter: clusterParameter1});

        newShard5.restart(0, {shardsvr: ""});
        newShard5.awaitNodesAgreeOnPrimary();

        checkClusterParameters(
            clusterParameter1Name,
            clusterParameter1Value,
            newShard5.getPrimary(),
            newShard5.getPrimary(),
        );

        assert.commandWorked(mongos.adminCommand({addShard: newShard5.getURL(), name: newShard5Name}));

        checkClusterParameters(
            clusterParameter2Name,
            clusterParameter2Value,
            configShard.getPrimary(),
            newShard5.getPrimary(),
        );

        assert.eq(
            0,
            newShard5.getPrimary().getCollection("config.clusterParameters").countDocuments({
                _id: clusterParameter1Name,
            }),
        );

        MongoRunner.stopMongos(mongos);
        newShard5.stopSet();
        configShard.stopSet();
    }
}
