/**
 * Checks that setClusterParameter command only run once
 *
 * @tags: [
 *   # Requires all nodes to be running the latest binary.
 *   requires_fcv_60,
 *   featureFlagClusterWideConfig,
 *   does_not_support_stepdowns
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

const st = new ShardingTest({shards: 1, rs: {nodes: 3}});

let fp =
    configureFailPoint(st.configRS.getPrimary(), 'hangBeforeRunningConfigsvrCoordinatorInstance');

let setClusterParameterSuccessThread = new Thread((mongosConnString, clusterParameter) => {
    let mongos = new Mongo(mongosConnString);
    assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
}, st.s.host, clusterParameter1);

setClusterParameterSuccessThread.start();
fp.wait();

jsTestLog(
    'Check that 2 requests for the same cluster parameter and same value generates only one coordinator.');

let setClusterParameterJoinSuccessThread = new Thread((mongosConnString, clusterParameter) => {
    let mongos = new Mongo(mongosConnString);
    assert.commandWorked(mongos.adminCommand({setClusterParameter: clusterParameter}));
}, st.s.host, clusterParameter1);

setClusterParameterJoinSuccessThread.start();
fp.wait();

let currOp =
    st.configRS.getPrimary()
        .getDB('admin')
        .aggregate(
            [{$currentOp: {allUsers: true}}, {$match: {desc: 'SetClusterParameterCoordinator'}}])
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

jsTestLog('Check forward progress until completion in the presence of a config server stepdown.');

fp = configureFailPoint(st.configRS.getPrimary(), 'hangBeforeRunningConfigsvrCoordinatorInstance');

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

const clusterParametersConfigColl =
    st.configRS.getPrimary().getCollection('config.clusterParameters');

const shardParametersConfigColl = st.rs0.getPrimary().getCollection('config.clusterParameters');

assert.eq(1, clusterParametersConfigColl.countDocuments({_id: clusterParameter2Name}));
const configClusterParameter = clusterParametersConfigColl.findOne(
    {_id: clusterParameter2Name}, {_id: 0, clusterParameterTime: 0});
const shardClusterParameter = shardParametersConfigColl.findOne({_id: clusterParameter2Name},
                                                                {_id: 0, clusterParameterTime: 0});
assert.docEq(configClusterParameter, clusterParameter2Value);
assert.docEq(shardClusterParameter, clusterParameter2Value);

fp.off();

// Check the full cluster has the same clusterParameterTime as the config server.
const configParameterTime =
    clusterParametersConfigColl.findOne({_id: clusterParameter2Name}, {clusterParameterTime: 1})
        .clusterParameterTime;
assert.eq(configParameterTime,
          shardParametersConfigColl.findOne({_id: clusterParameter2Name}, {clusterParameterTime: 1})
              .clusterParameterTime);

st.stop();
})();
