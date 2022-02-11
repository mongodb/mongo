/**
 * Tests the collectionUUID parameter of the aggregate command for $indexStats and $collStats
 * pipelines.
 */
(function() {
'use strict';

const validateErrorResponse = function(res, collectionUUID, expectedNamespace, actualNamespace) {
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedNamespace, expectedNamespace);
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
    validateErrorResponse(res, nonexistentUUID, coll.getFullName(), null);

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection.");
    const coll2 = testDB['coll_2'];
    assert.commandWorked(coll2.insert({x: 1, y: 1}));
    cmdObj[cmd] = coll2.getName();
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll2.getFullName(), coll.getFullName());

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace does not exist.");
    coll2.drop();
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, coll2.getFullName(), coll.getFullName());

    jsTestLog("The command '" + cmd + "' succeeds on view when no UUID is provided.");
    const viewName = "view";
    assert.commandWorked(testDB.runCommand(
        {create: viewName, viewOn: coll.getName(), pipeline: [], writeConcern: {w: "majority"}}));

    cmdObj[cmd] = viewName;
    delete cmdObj.collectionUUID;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace is a view.");
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, testDB.getName() + '.' + viewName, coll.getFullName());
    assert.commandWorked(testDB.runCommand({drop: viewName, writeConcern: {w: "majority"}}));

    jsTestLog("The command '" + cmd +
              "' succeeds on timeseries collection when no UUID is provided.");
    const tsCollName = "ts_coll";
    assert.commandWorked(testDB.runCommand(
        {create: tsCollName, timeseries: {timeField: 'time'}, writeConcern: {w: "majority"}}));
    cmdObj[cmd] = tsCollName;
    delete cmdObj.collectionUUID;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace is a timeseries collection.");
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, uuid, testDB.getName() + '.' + tsCollName, coll.getFullName());
    assert.commandWorked(testDB.runCommand({drop: tsCollName, writeConcern: {w: "majority"}}));
};

testCommand("aggregate", {aggregate: "", pipeline: [{$indexStats: {}}], cursor: {}});
testCommand("aggregate", {aggregate: "", pipeline: [{$collStats: {latencyStats: {}}}], cursor: {}});
})();
