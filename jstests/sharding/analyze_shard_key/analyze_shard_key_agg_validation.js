/**
 * Tests that basic validation within the $_analyzeShardKeyReadWriteDistribution aggregate stage.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/config_shard_util.js");
load("jstests/sharding/analyze_shard_key/libs/validation_common.js");

function makeAnalyzeShardKeyAggregateCmdObj(collName, key, splitPointsShardId) {
    const commandId = UUID();
    const spec = {
        key,
        splitPointsFilter: {"_id.commandId": commandId},
        splitPointsAfterClusterTime: new Timestamp(100, 1),
    };
    if (splitPointsShardId) {
        spec["splitPointsShardId"] = splitPointsShardId;
    }
    return {
        aggCmdObj: {
            aggregate: collName,
            pipeline: [{$_analyzeShardKeyReadWriteDistribution: spec}],
            cursor: {}
        },
        makeSplitPointIdFunc: () => {
            return {commandId, splitPointId: UUID()};
        }
    };
}

function runTest(rst, validationTest, shardName) {
    const primary = rst.getPrimary();
    const splitPointsColl = primary.getCollection("config.analyzeShardKeySplitPoints");

    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(`Testing that the aggregation stage fails if the namespace is invalid ${
            tojson({dbName, collName})}`);
        const {aggCmdObj} = makeAnalyzeShardKeyAggregateCmdObj(collName, {_id: 1}, shardName);
        const res = primary.getDB(dbName).runCommand(aggCmdObj);
        if (isView) {
            // Running an aggregation stage on view is equivalent to running the aggregation on the
            // collection so no CommandNotSupportedOnView error would be thrown.
            assert.commandWorked(res);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
        }
    }

    for (let key of validationTest.invalidShardKeyTestCases) {
        jsTest.log(`Testing that the aggregation stage fails if the shard key is invalid ${
            tojson({key})}`);
        const {aggCmdObj} =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, key, shardName);
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point with an" +
                   " array field");
        const ns = validationTest.dbName + "." + validationTest.collName;
        const {aggCmdObj, makeSplitPointIdFunc} =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {b: 1}, shardName);
        assert.commandWorked(splitPointsColl.insert(
            {_id: makeSplitPointIdFunc(), ns, splitPoint: {b: [0, 0]}, expireAt: new Date()}));
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
        assert.commandWorked(splitPointsColl.remove({}));
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point that does" +
                   " not have the same pattern as the shard key");
        const ns = validationTest.dbName + "." + validationTest.collName;
        const {aggCmdObj, makeSplitPointIdFunc} =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {a: 1}, shardName);
        assert.commandWorked(splitPointsColl.insert(
            {_id: makeSplitPointIdFunc(), ns, splitPoint: {_id: 1}, expireAt: new Date()}));
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
        assert.commandWorked(splitPointsColl.remove({}));
    }
}

{
    const st = new ShardingTest({shards: 1});
    const shard0Primary = st.rs0.getPrimary();
    const configPrimary = st.configRS.getPrimary();
    const validationTest = ValidationTest(st.s);

    {
        jsTest.log("Testing that the aggregation stage is supported on shardsvr mongod but not on" +
                   " configsvr mongod");
        const {aggCmdObj} =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {id: 1}, st.shard0.name);
        assert.commandWorked(shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj));
        if (!ConfigShardUtil.isEnabledIgnoringFCV(st)) {
            assert.commandFailedWithCode(
                configPrimary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                ErrorCodes.IllegalOperation);
        }
    }

    runTest(st.rs0, validationTest, st.shard0.name);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const validationTest = ValidationTest(primary);

    jsTest.log(
        "Testing that the aggregation stage is supported on a standalone replica set mongod");
    runTest(rst, validationTest, null /* shardName */);

    rst.stopSet();
}
})();
