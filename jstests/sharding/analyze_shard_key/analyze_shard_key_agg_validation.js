/**
 * Tests that basic validation within the $_analyzeShardKeyReadWriteDistribution aggregate stage.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/catalog_shard_util.js");
load("jstests/sharding/analyze_shard_key/libs/validation_common.js");

function makeAnalyzeShardKeyAggregateCmdObj(collName, key, splitPointsShardId) {
    const spec = {
        key,
        splitPointsNss: "config.analyzeShardKey.splitPoints.uuid0.uuid1",
        splitPointsAfterClusterTime: new Timestamp(100, 1),
    };
    if (splitPointsShardId) {
        spec["splitPointsShardId"] = splitPointsShardId;
    }
    return {
        aggregate: collName,
        pipeline: [{$_analyzeShardKeyReadWriteDistribution: spec}],
        cursor: {}
    };
}

function runTest(rst, validationTest, shardName) {
    const primary = rst.getPrimary();
    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(`Testing that the aggregation stage fails if the namespace is invalid ${
            tojson({dbName, collName})}`);
        const aggCmdObj = makeAnalyzeShardKeyAggregateCmdObj(collName, {_id: 1}, shardName);
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
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, key, shardName);
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point with an" +
                   " array field");
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {b: 1}, shardName);
        const splitPointsColl = primary.getCollection(
            aggCmdObj.pipeline[0]["$_analyzeShardKeyReadWriteDistribution"].splitPointsNss);
        assert.commandWorked(splitPointsColl.insert({_id: UUID(), splitPoint: {b: [0, 0]}}));
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
        splitPointsColl.drop();
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point that does" +
                   " not have the same pattern as the shard key");
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {a: 1}, shardName);
        const splitPointsColl = primary.getCollection(
            aggCmdObj.pipeline[0]["$_analyzeShardKeyReadWriteDistribution"].splitPointsNss);
        assert.commandWorked(splitPointsColl.insert({_id: UUID(), splitPoint: {_id: 1}}));
        assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                     ErrorCodes.BadValue);
        splitPointsColl.drop();
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
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(validationTest.collName, {id: 1}, st.shard0.name);
        assert.commandWorked(shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj));
        if (!CatalogShardUtil.isEnabledIgnoringFCV(st)) {
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
