/**
 * Utility functions for upgrade_enables_extension_foo.js and
 * upgrade_enables_extension_foo_auth.js.
 *
 * TODO SERVER-109450 Delete this file once auth and non-auth have the same behavior.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");

const fooStageUnrecognizedErrCode = 40324;
export const fooExtensionNodeOptions = {
    loadExtensions: [pathToExtensionFoo],
    setParameter: {featureFlagExtensionsAPI: true},
};

const collName = jsTestName();
const viewName = "foo_view";
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const docs = [
    {_id: 0, foo: "dreaming"},
    {_id: 1, foo: "about"},
    {_id: 2, foo: "super cool extensions"},
];

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
        fooStageUnrecognizedErrCode,
    );

    // $testFoo is rejected in a view pipeline.
    assert.commandFailedWithCode(db.createView(viewName, collName, [{$testFoo: {}}]), fooStageUnrecognizedErrCode);
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
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});
}

export function assertFooViewCreationAllowedButQueriesRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // This behavior is flaky. Most of the time here, $testFoo is permitted in a pipeline to
    // createView(), but you cannot run queries over that view yet. Occasionally, however,
    // the createView() command is rejected first for not recognizing $testFoo.
    const createViewResult = db.createView(viewName, collName, [{$testFoo: {}}]);
    if (createViewResult["ok"]) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: viewName, pipeline: [], cursor: {}}),
            fooStageUnrecognizedErrCode,
        );
    } else {
        assert.commandFailedWithCode(createViewResult, fooStageUnrecognizedErrCode);
    }

    // $testFoo is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}}),
        fooStageUnrecognizedErrCode,
    );
}
