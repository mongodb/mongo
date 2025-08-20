/**
 * This test simulates a scenario where you're upgrading from a MongoDB version with no extensions
 * to a loaded to a higher MongoDB version with an extension loaded. We test this expected behavior:
 *    - The nodes are spun up at a lower version with no extensions loaded. All $testFoo queries
 *      should be rejected, both in an agg command and in a createView pipeline.
 *    - In a ReplSet, $testFoo queries should be accepted once all binaries are upgraded (the
 *      primary last). Nothing is dependent on FCV. Note that we are missing coverage for ReplSet
 *      upgrade/downgrade where we aren't just connected to the primary the whole time
 *      (TODO SERVER-109457).
 *    - In a sharded cluster, $testFoo queries should be accepted once all binaries are upgraded
 *      (the mongos last), and again nothing is dependent on FCV.
 *       * However, there is one difference: in the period between when the shards start restarting
 *         until mongos has restarted, the cluster may allow you to create a view with $testFoo
 *         even though you cannot run queries on the view until mongos has upgraded. This behavior
 *         is flaky, as it occasionally rejets the view creation to begin with. Note that this only
 *         happens when auth is not enabed (see upgrade_enables_extension_foo_auth.js for testing
 *         with more consistent behavior when auth is enabled).
 *
 * TODO SERVER-109457 Investigate more ReplSet scenarios.
 *
 * TODO SERVER-109450 Auth-off cluster should have same behavior as auth-on cluster.
 * Extensions are only available on Linux machines.
 * @tags: [
 *   auth_incompatible,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");

const fooStageUnrecognizedErrCode = 40324;
export const fooExtensionNodeOptions = {
    loadExtensions: [pathToExtensionFoo],
    setParameter: {featureFlagExtensionsAPI: true}
};

const collName = jsTestName();
const viewName = "foo_view";
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const docs =
    [{_id: 0, foo: "dreaming"}, {_id: 1, foo: "about"}, {_id: 2, foo: "super cool extensions"}];

export function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        // Enable sharding on this collection by _id. After inserting the data below,
        // this will distribute 2 documents to one shard and 1 document to the other.
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(docs));
}

export function assertFooStageRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}}),
        fooStageUnrecognizedErrCode);

    // $testFoo is rejected in a view pipeline.
    assert.commandFailedWithCode(db.createView(viewName, collName, [{$testFoo: {}}]),
                                 fooStageUnrecognizedErrCode);
}

export function assertFooStageAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}));
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});
}

function assertFooViewCreationAllowedButQueriesRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // This behavior is flaky. Most of the time here, $testFoo is permitted in a pipeline to
    // createView(), but you cannot run queries over that view yet. Occasionally, however,
    // the createView() command is rejected first for not recognizing $testFoo.
    const createViewResult = db.createView(viewName, collName, [{$testFoo: {}}]);
    if (createViewResult["ok"]) {
        assert.commandFailedWithCode(db.runCommand({aggregate: viewName, pipeline: [], cursor: {}}),
                                     fooStageUnrecognizedErrCode);
    } else {
        assert.commandFailedWithCode(createViewResult, fooStageUnrecognizedErrCode);
    }

    // $testFoo is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}}),
        fooStageUnrecognizedErrCode);
}

testPerformUpgradeReplSet({
    upgradeNodeOptions: fooExtensionNodeOptions,
    setupFn: setupCollection,
    whenFullyDowngraded: assertFooStageRejected,
    whenSecondariesAreLatestBinary: assertFooStageRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
    whenFullyUpgraded: assertFooStageAccepted,
});

testPerformUpgradeSharded({
    upgradeNodeOptions: fooExtensionNodeOptions,
    setupFn: setupCollection,
    whenFullyDowngraded: assertFooStageRejected,
    whenOnlyConfigIsLatestBinary: assertFooStageRejected,
    whenSecondariesAndConfigAreLatestBinary: assertFooViewCreationAllowedButQueriesRejected,
    whenMongosBinaryIsLastLTS: assertFooViewCreationAllowedButQueriesRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
    whenFullyUpgraded: assertFooStageAccepted,
});
