/**
 * Tests the collectionUUID parameter of the aggregate command for $indexStats and $collStats
 * pipelines.
 */
(function() {
'use strict';

load("jstests/libs/fixture_helpers.js");  // For 'isMongos'

if (FixtureHelpers.isMongos(db)) {
    // TODO (SERVER-62563): Add the mongos test coverage.
    return;
}

const validateErrorResponse = function(res, collectionUUID, actualNamespace) {
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.actualNamespace, actualNamespace);
};

const testCommand = function(cmd, cmdObj) {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB['coll'];
    assert.commandWorked(coll.insert({x: 1, y: 2}));
    assert.commandWorked(coll.createIndex({y: 1}));
    const uuid = assert.commandWorked(testDB.runCommand({listCollections: 1}))
                     .cursor.firstBatch[0]
                     .info.uuid;

    jsTestLog("The command '" + cmd + "' succeeds when the correct UUID is provided.");
    cmdObj[cmd] = coll.getName();
    cmdObj["collectionUUID"] = uuid;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID does not correspond to an existing collection.");
    const nonexistentUUID = UUID();
    cmdObj["collectionUUID"] = nonexistentUUID;
    let res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, nonexistentUUID, "");

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection.");
    const coll2 = testDB['coll_2'];
    assert.commandWorked(coll2.insert({x: 1, y: 1}));
    cmdObj[cmd] = coll2.getName();
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll.getFullName());

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace does not exist.");
    coll2.drop();
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll.getFullName());

    jsTestLog("The command '" + cmd + "' succeeds on view when no UUID is provided.");
    const view = testDB['view'];
    assert.commandWorked(testDB.createView(view.getName(), coll.getName(), []));
    cmdObj[cmd] = view.getName();
    delete cmdObj.collectionUUID;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace is a view.");
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll.getFullName());

    jsTestLog("The command '" + cmd +
              "' succeeds on timeseries collection when no UUID is provided.");
    const tsColl = testDB["ts_coll"];
    assert.commandWorked(
        testDB.createCollection(tsColl.getName(), {timeseries: {timeField: 'time'}}));
    cmdObj[cmd] = tsColl.getName();
    delete cmdObj.collectionUUID;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace is a timeseries collection.");
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll.getFullName());
};

testCommand("aggregate", {aggregate: "", pipeline: [{$indexStats: {}}], cursor: {}});
testCommand("aggregate", {aggregate: "", pipeline: [{$collStats: {latencyStats: {}}}], cursor: {}});
})();
