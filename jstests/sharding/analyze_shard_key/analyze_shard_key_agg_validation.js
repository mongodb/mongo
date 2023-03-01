/**
 * Tests that basic validation within the $_analyzeShardKeyReadWriteDistribution aggregate stage.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/catalog_shard_util.js");
load("jstests/sharding/analyze_shard_key/libs/validation_common.js");

function makeAnalyzeShardKeyAggregateCmdObj(st, collName, key, splitPointsShardId) {
    return {
        aggregate: collName,
        pipeline: [{
            $_analyzeShardKeyReadWriteDistribution: {
                key,
                splitPointsShardId: splitPointsShardId ? splitPointsShardId
                                                       : (st ? st.shard0.name : "dummyShardId"),
                splitPointsNss: "config.analyzeShardKey.splitPoints.uuid0.uuid1",
                splitPointsAfterClusterTime: new Timestamp(100, 1),
            }
        }],
        cursor: {}
    };
}

{
    const st = new ShardingTest({shards: 1});
    const shard0Primary = st.rs0.getPrimary();
    const configPrimary = st.configRS.getPrimary();
    const validationTest = ValidationTest(st.s);

    {
        jsTest.log("Testing that the aggregation stage is supported on shardsvr mongod but not on" +
                   " configsvr mongod");
        const aggCmdObj = makeAnalyzeShardKeyAggregateCmdObj(
            st, validationTest.collName, {id: 1}, st.shard0.name);
        assert.commandWorked(shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj));
        if (!CatalogShardUtil.isEnabledIgnoringFCV(st)) {
            assert.commandFailedWithCode(
                configPrimary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                ErrorCodes.IllegalOperation);
        }
    }

    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(`Testing that the aggregation stage fails if the namespace is invalid ${
            tojson({dbName, collName})}`);
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(st, collName, {_id: 1}, st.shard0.name);
        const res = shard0Primary.getDB(dbName).runCommand(aggCmdObj);
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
            makeAnalyzeShardKeyAggregateCmdObj(st, validationTest.collName, key, st.shard0.name);
        assert.commandFailedWithCode(
            shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj), ErrorCodes.BadValue);
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point with an" +
                   " array field");
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(st, validationTest.collName, {b: 1}, st.shard0.name);
        const splitPointsColl = shard0Primary.getCollection(
            aggCmdObj.pipeline[0]["$_analyzeShardKeyReadWriteDistribution"].splitPointsNss);
        assert.commandWorked(splitPointsColl.insert({splitPoint: {b: [0, 0]}}));
        assert.commandFailedWithCode(
            shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj), ErrorCodes.BadValue);
        splitPointsColl.drop();
    }

    {
        jsTest.log("Testing that the aggregation stage fails if there is a split point that does" +
                   " not have the same pattern as the shard key");
        const aggCmdObj =
            makeAnalyzeShardKeyAggregateCmdObj(st, validationTest.collName, {a: 1}, st.shard0.name);
        const splitPointsColl = shard0Primary.getCollection(
            aggCmdObj.pipeline[0]["$_analyzeShardKeyReadWriteDistribution"].splitPointsNss);
        assert.commandWorked(splitPointsColl.insert({splitPoint: {_id: 1}}));
        assert.commandFailedWithCode(
            shard0Primary.getDB(validationTest.dbName).runCommand(aggCmdObj), ErrorCodes.BadValue);
        splitPointsColl.drop();
    }

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const validationTest = ValidationTest(primary);

    jsTest.log("Testing that the aggregation stage is not supported on a replica set");
    const aggCmdObj =
        makeAnalyzeShardKeyAggregateCmdObj(null /* st */, validationTest.collName, {id: 1});
    assert.commandFailedWithCode(primary.getDB(validationTest.dbName).runCommand(aggCmdObj),
                                 ErrorCodes.IllegalOperation);

    rst.stopSet();
}
})();
