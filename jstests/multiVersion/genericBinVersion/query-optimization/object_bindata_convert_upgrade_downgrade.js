/**
 * Verifies that $convert from Object to BinData behaves correctly in FCV upgrade/downgrade
 * scenarios.
 *
 * @tags: [
 *   featureFlagConvertObjectToBinData,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collectionName = "coll";
const viewName = "objectToBinDataView";

const testCase = {
    docs: [
        {_id: 0, input: {a: "a"}},
        {_id: 1, input: {a: "a"}},
    ],
    pipeline: [{$project: {_id: 0, output: {$convert: {input: "$input", to: "binData"}}}}],
    result: [{output: BinData(0, "DgAAAAJhAAIAAABhAAA=")}, {output: BinData(0, "DgAAAAJhAAIAAABhAAA=")}],
};

const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

function setupCollection(primaryConnection, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConnection), collectionName);

    if (shardingTest) {
        // Shard on _id to exercise the conversion on both shards.
        shardingTest.shardColl(coll, {_id: 1}, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(testCase.docs));

    if (shardingTest) {
        // Verify that documents are distributed across both shards.
        const shardCounts = shardingTest.shardCounts(jsTestName(), collectionName);
        assert.eq(shardCounts[0], 1);
        assert.eq(shardCounts[1], 1);
    }
}

function assertCreateViewAndEvaluateViewOrAggregateFail(primaryConnection) {
    const db = getDB(primaryConnection);

    // The view can be created (existing $convert syntax parses fine on older binaries) but
    // evaluating it should fail with ConversionFailure.
    db[viewName].drop();
    assert.commandWorked(db.createView(viewName, collectionName, testCase.pipeline));
    assert.commandFailedWithCode(db.runCommand({find: viewName, filter: {}}), ErrorCodes.ConversionFailure);

    assert.commandFailedWithCode(
        db.runCommand({aggregate: collectionName, cursor: {}, pipeline: testCase.pipeline}),
        ErrorCodes.ConversionFailure,
    );
}

function assertCreateAndEvaluateViewOrAggregatePass(primaryConnection) {
    const db = getDB(primaryConnection);

    // The view created in the previous (FCV-downgraded) phase should start succeeding once the FCV
    // bump makes the fcv-gated feature flag effective, and produce the expected BSON encoding.
    assert.eq(testCase.result, db[viewName].find().toArray());

    db[viewName].drop();
    assert.commandWorked(db.createView(viewName, collectionName, testCase.pipeline));
    assert.eq(testCase.result, db[viewName].find().toArray());
    assert.eq(testCase.result, db[collectionName].aggregate(testCase.pipeline).toArray());
}

testPerformUpgradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenSecondariesAreLatestBinary: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenBinariesAreLatestAndFCVIsLastLTS: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenFullyUpgraded: assertCreateAndEvaluateViewOrAggregatePass,
});

testPerformUpgradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenOnlyConfigIsLatestBinary: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenSecondariesAndConfigAreLatestBinary: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenMongosBinaryIsLastLTS: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenBinariesAreLatestAndFCVIsLastLTS: assertCreateViewAndEvaluateViewOrAggregateFail,
    whenFullyUpgraded: assertCreateAndEvaluateViewOrAggregatePass,
});
