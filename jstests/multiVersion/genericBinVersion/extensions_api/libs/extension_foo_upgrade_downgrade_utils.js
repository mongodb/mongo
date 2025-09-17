/**
 * Utility functions for extension_foo_upgrade_downgrade.js and
 * extension_foo_upgrade_downgrade_auth.js.
 *
 * TODO SERVER-109450 Delete this file once auth and non-auth have the same behavior.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {generateExtensionConfigs, deleteExtensionConfigs} from "jstests/noPassthrough/libs/extension_helpers.js";

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

export function generateMultiversionExtensionConfigs() {
    return generateExtensionConfigs([pathToExtensionFoo, pathToExtensionFooV2]);
}

export function deleteMultiversionExtensionConfigs(extensionPaths) {
    deleteExtensionConfigs(extensionPaths);
}

export function extensionNodeOptions(extensionPath) {
    return {
        loadExtensions: [extensionPath],
        setParameter: {featureFlagExtensionsAPI: true},
    };
}

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

export function assertFooStageAcceptedV1OnlyPlusV2ViewCreation(primaryConn) {
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
