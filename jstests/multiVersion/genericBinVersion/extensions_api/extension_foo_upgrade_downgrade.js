/**
 * This test simulates a scenario when you're upgrading/downgrading an extension between V1 and V2
 * while maintaining the same server version. For the sake of testing, $testFoo in V1 must provide
 * an empty stage definition like {$testFoo: {}} or it will reject the query at parsing; $testFoo in
 * V2 loosens those restrictions. We test this expected behavior for upgrade:
 *     - The nodes are spun up with extension V1 loaded. $testFoo is accepted in queries and views,
 *       but it is always rejected (in queries and views) with a non-empty stage definition.
 *     - In a ReplSet, $testFoo V1 queries must be accepted the whole time. $testFoo V2 queries
 *       are only accepted once all binaries are upgraded (the primary last). Note that we are
 *       missing coverage for ReplSet upgrade/downgrade where we aren't just connected to the
 *       primary the whole time (TODO SERVER-109457).
 *     - In a sharded cluster, again $testFoo V1 queries must be accepted the whole time, and
 *       $testFoo V2 queries are only accepted once all binaries are upgraded (the mongos last).
 *        * However, there is one difference: in the period between when the shards start restarting
 *          until mongos has restarted, the cluster may allow you to create a view with $testFoo V2
 *          even though you cannot run queries on the view until mongos has upgraded. This behavior
 *          is flaky, as it occasionally rejects the view creation to begin with. Note that this
 *          only happens when auth is not enabled (see extension_foo_upgrade_downgrade_auth.js for
 *          testing with more consistent behavior when auth is enabled).
 *     - The downgrade behavior is reverse and more consistent (no flaky view issues).
 *
 * This test is technically not multiversion since we only use latest binaries, but it stays with
 * multiversion tests since we use the upgrade/downgrade library utils.
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
import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/multi_cluster.js";

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {testPerformReplSetRollingRestart} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformShardedClusterRollingRestart} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");
const pathToExtensionFooV2 = MongoRunner.getExtensionPath("libfoo_extension_v2.so");

const collName = jsTestName();
const viewName = "foo_view";
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const data = [
    {_id: 0, test: "a"},
    {_id: 1, test: "b"},
    {_id: 2, test: "c"},
];

const fooParseErrorCodes = [10624200, 10624201];

export const fooV1Options = {
    loadExtensions: [pathToExtensionFoo],
    setParameter: {featureFlagExtensionsAPI: true},
};
export const fooV2Options = {
    loadExtensions: [pathToExtensionFooV2],
    setParameter: {featureFlagExtensionsAPI: true},
};

export function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);
    if (shardingTest) {
        // Enable sharding on this collection by _id. After inserting the data below,
        // this will distribute 2 documents to one shard and 1 document to the other.
        shardingTest.shardColl(coll, {_id: 1});
    }
    assert.commandWorked(coll.insertMany(data));
}

export function assertFooStageAcceptedV1Only(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo in v1 should reject a non-empty stage definition.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}}),
        fooParseErrorCodes,
    );

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: data});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [data[0]]});
}

export function assertFooStageAcceptedV1AndV2(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    let result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: data});

    // $testFoo V2 now accepts a non-empty stage definition.
    result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: data});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    let viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [data[0]]});

    db[viewName].drop();

    // $testFoo with a non-empty stage definition is now accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {nonEmpty: true}}]));
    viewResult = assert.commandWorked(db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}));
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [data[0]]});
}

function assertFooStageAcceptedV1OnlyPlusV2ViewCreation(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo should reject a non-empty stage definition in an aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}}),
        fooParseErrorCodes,
    );

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: data});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [data[0]]});

    // This behavior is flaky. Most of the time here, $testFoo with a non-empty stage definition is
    // allowed in a view pipeline, but queries over that view cannot yet be run. Occasionally,
    // however, the createView() command is rejected first for not recognizing $testFoo V2 yet.
    db[viewName].drop();
    const createViewResult = db.createView(viewName, collName, [{$testFoo: {nonEmpty: true}}]);
    if (createViewResult["ok"]) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: viewName, pipeline: [], cursor: {}}),
            fooParseErrorCodes,
        );
    } else {
        assert.commandFailedWithCode(createViewResult, fooParseErrorCodes);
    }
}

// Test upgrading foo extension in a replica set.
testPerformReplSetRollingRestart({
    startingNodeOptions: fooV1Options,
    restartNodeOptions: fooV2Options,
    setupFn: setupCollection,
    beforeRestart: assertFooStageAcceptedV1Only,
    afterSecondariesHaveRestarted: assertFooStageAcceptedV1Only,
    afterPrimariesHaveRestarted: assertFooStageAcceptedV1AndV2,
});

// Test upgrading foo extension in a sharded cluster.
testPerformShardedClusterRollingRestart({
    startingNodeOptions: fooV1Options,
    restartNodeOptions: fooV2Options,
    setupFn: setupCollection,
    beforeRestart: assertFooStageAcceptedV1Only,
    afterConfigHasRestarted: assertFooStageAcceptedV1Only,
    afterSecondaryShardHasRestarted: assertFooStageAcceptedV1OnlyPlusV2ViewCreation,
    afterPrimaryShardHasRestarted: assertFooStageAcceptedV1OnlyPlusV2ViewCreation,
    afterMongosHasRestarted: assertFooStageAcceptedV1AndV2,
});

// Test downgrading foo extension in a replica set.
testPerformReplSetRollingRestart({
    startingNodeOptions: fooV2Options,
    restartNodeOptions: fooV1Options,
    setupFn: setupCollection,
    beforeRestart: assertFooStageAcceptedV1AndV2,
    afterSecondariesHaveRestarted: assertFooStageAcceptedV1AndV2,
    afterPrimariesHaveRestarted: assertFooStageAcceptedV1Only,
});

// Test downgrading foo extension in a sharded cluster.
testPerformShardedClusterRollingRestart({
    startingNodeOptions: fooV2Options,
    restartNodeOptions: fooV1Options,
    setupFn: setupCollection,
    beforeRestart: assertFooStageAcceptedV1AndV2,
    afterConfigHasRestarted: assertFooStageAcceptedV1AndV2,
    afterSecondaryShardHasRestarted: assertFooStageAcceptedV1Only,
    afterPrimaryShardHasRestarted: assertFooStageAcceptedV1Only,
    afterMongosHasRestarted: assertFooStageAcceptedV1Only,
});
