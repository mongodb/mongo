/**
 * Checks that setClusterParameter command only run once
 *
 * We have a restart in the test with some stored values that must be preserved so it cannot run in
 * inMemory variants
 *
 * @tags: [
 *   # Requires all nodes to be running the latest binary.
 *   requires_fcv_60,
 *   featureFlagClusterWideConfig,
 *   does_not_support_stepdowns,
 *   requires_persistence,
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

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
const clusterParameter3Name = 'testBoolClusterParameter';
const clusterParameter3 = {
    [clusterParameter3Name]: clusterParameter3Value
};

const checkClusterParameters =
    (clusterParameterName, clusterParameterValue, configRS, shard, checkTime = true) => {
        const shardParametersConfigColl = shard.getCollection('config.clusterParameters');
        const clusterParametersConfigColl = configRS.getCollection('config.clusterParameters');

        assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameterName}));
        const configClusterParameter = clusterParametersConfigColl.findOne(
            {_id: clusterParameterName}, {_id: 0, clusterParameterTime: 0});
        const shardClusterParameter = shardParametersConfigColl.findOne(
            {_id: clusterParameterName}, {_id: 0, clusterParameterTime: 0});
        assert.docEq(configClusterParameter, clusterParameterValue);
        assert.docEq(shardClusterParameter, clusterParameterValue);

        if (checkTime) {
            // Check the full cluster has the same clusterParameterTime as the config server.
            const configParameterTime =
                clusterParametersConfigColl
                    .findOne({_id: clusterParameterName}, {clusterParameterTime: 1})
                    .clusterParameterTime;
            assert.eq(configParameterTime,
                      shardParametersConfigColl
                          .findOne({_id: clusterParameterName}, {clusterParameterTime: 1})
                          .clusterParameterTime);
        }
    };

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 3}});

    {
        jsTestLog(
            'Check that 2 requests for the same cluster parameter and same value generates only one coordinator.');

        let fp = configureFailPoint(st.configRS.getPrimary(),
                                    'hangBeforeRunningConfigsvrCoordinatorInstance');

        let setClusterParameterSuccessThread = new Thread((mongosConnString, clusterParameter) => {
            let mongos = new Mongo(mongosConnString);
            assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
        }, st.s.host, clusterParameter1);

        setClusterParameterSuccessThread.start();
        fp.wait();

        let setClusterParameterJoinSuccessThread =
            new Thread((mongosConnString, clusterParameter) => {
                let mongos = new Mongo(mongosConnString);
                assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
            }, st.s.host, clusterParameter1);

        setClusterParameterJoinSuccessThread.start();
        fp.wait();

        let currOp = st.configRS.getPrimary()
                         .getDB('admin')
                         .aggregate([
                             {$currentOp: {allUsers: true}},
                             {$match: {desc: 'SetClusterParameterCoordinator'}}
                         ])
                         .toArray();
        assert.eq(1, currOp.length);
        assert(currOp[0].hasOwnProperty('command'));
        assert.docEq(currOp[0].command, clusterParameter1);

        jsTestLog('Check that a second request will fail with ConflictingOperationInProgress.');

        assert.commandFailedWithCode(st.s.adminCommand({setClusterParameter: clusterParameter2}),
                                     ErrorCodes.ConflictingOperationInProgress);

        fp.off();
        setClusterParameterSuccessThread.join();
        setClusterParameterJoinSuccessThread.join();
    }

    {
        jsTestLog(
            'Check forward progress until completion in the presence of a config server stepdown.');

        let fp = configureFailPoint(st.configRS.getPrimary(),
                                    'hangBeforeRunningConfigsvrCoordinatorInstance');

        let setClusterParameterThread = new Thread((mongosConnString, clusterParameter) => {
            let mongos = new Mongo(mongosConnString);
            assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
        }, st.s.host, clusterParameter2);

        setClusterParameterThread.start();
        fp.wait();

        let newPrimary = st.configRS.getSecondary();

        st.configRS.stepUp(newPrimary);

        // After the stepdown the command should be retried and finish successfully.
        setClusterParameterThread.join();

        checkClusterParameters(clusterParameter2Name,
                               clusterParameter2Value,
                               st.configRS.getPrimary(),
                               st.rs0.getPrimary());

        fp.off();
    }

    {
        jsTestLog('Check that addShard serializes with setClusterParameter.');

        const newShardName = 'newShard';
        const newShard = new ReplSetTest({name: newShardName, nodes: 1});
        newShard.startSet({shardsvr: ''});
        newShard.initiate();

        // Failing _shardsvrSetclusterParameter with a retriable error will make the configsvr
        // coordinator retry until completion.
        let shardsvrSetClusterParameterFailPoint =
            configureFailPoint(st.rs0.getPrimary(), 'failCommand', {
                failInternalCommands: true,
                errorCode: ErrorCodes.PrimarySteppedDown,
                failCommands: ["_shardsvrSetClusterParameter"]
            });

        let setClusterParameterThread = new Thread((mongosConnString, clusterParameter) => {
            let mongos = new Mongo(mongosConnString);
            assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
        }, st.s.host, clusterParameter3);
        setClusterParameterThread.start();

        shardsvrSetClusterParameterFailPoint.wait();

        assert.commandFailedWithCode(
            st.s.adminCommand({addShard: newShard.getURL(), name: newShardName, maxTimeMS: 1000}),
            ErrorCodes.MaxTimeMSExpired);

        shardsvrSetClusterParameterFailPoint.off();
        setClusterParameterThread.join();

        jsTestLog('Check that the config server will push all parameters when adding a new shard.');

        assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));

        checkClusterParameters(clusterParameter1Name,
                               clusterParameter1Value,
                               st.configRS.getPrimary(),
                               newShard.getPrimary());
        checkClusterParameters(clusterParameter2Name,
                               clusterParameter2Value,
                               st.configRS.getPrimary(),
                               newShard.getPrimary());
        checkClusterParameters(clusterParameter3Name,
                               clusterParameter3Value,
                               st.configRS.getPrimary(),
                               newShard.getPrimary());

        assert.soon(() => {
            let res = st.s.adminCommand({removeShard: newShardName});
            return res.state == 'completed';
        });

        newShard.stopSet();

        // Final check, the initial shard has all the cluster parameters
        checkClusterParameters(clusterParameter1Name,
                               clusterParameter1Value,
                               st.configRS.getPrimary(),
                               st.rs0.getPrimary());
        checkClusterParameters(clusterParameter2Name,
                               clusterParameter2Value,
                               st.configRS.getPrimary(),
                               st.rs0.getPrimary());
        checkClusterParameters(clusterParameter3Name,
                               clusterParameter3Value,
                               st.configRS.getPrimary(),
                               st.rs0.getPrimary());
    }

    st.stop();
}

{
    jsTestLog('Check that parameters added in cluster overwrite custom RS parameters.');
    const st2 = new ShardingTest({mongos: 1, shards: 0, name: 'second_cluster'});

    // Set clusterParameter1 on the cluster with no shards, it will be registered in the config
    // server.
    assert.commandWorked(st2.s.adminCommand({setClusterParameter: clusterParameter1}));
    checkClusterParameters(clusterParameter1Name,
                           clusterParameter1Value,
                           st2.configRS.getPrimary(),
                           st2.configRS.getPrimary());

    const newShard2Name = 'newShard2';
    const newShard2 = new ReplSetTest({name: newShard2Name, nodes: 1});
    newShard2.startSet();
    newShard2.initiate();

    // Set clusterParameter2 on the replica set.
    assert.commandWorked(
        newShard2.getPrimary().adminCommand({setClusterParameter: clusterParameter2}));

    newShard2.restart(0, {shardsvr: ''});
    newShard2.awaitNodesAgreeOnPrimary();

    checkClusterParameters(clusterParameter2Name,
                           clusterParameter2Value,
                           newShard2.getPrimary(),
                           newShard2.getPrimary(),
                           false);

    assert.commandWorked(st2.s.adminCommand({addShard: newShard2.getURL(), name: newShard2Name}));

    // After adding the shard there must be only one cluster parameter: the one set on the cluster
    // clusterParameter1.
    checkClusterParameters(clusterParameter1Name,
                           clusterParameter1Value,
                           st2.configRS.getPrimary(),
                           newShard2.getPrimary());
    assert.eq(0, newShard2.getPrimary().getCollection('config.clusterParameters').countDocuments({
        _id: clusterParameter2Name
    }));

    newShard2.stopSet();

    st2.stop();
}
})();
