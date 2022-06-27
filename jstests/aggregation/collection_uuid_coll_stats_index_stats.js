/**
 * Tests the collectionUUID parameter of the aggregate command for $indexStats and $collStats
 * pipelines.
 */
(function() {
'use strict';

const validateErrorResponse = function(
    res, db, collectionUUID, expectedCollection, actualCollection) {
    assert.eq(res.db, db);
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, expectedCollection);
    assert.eq(res.actualCollection, actualCollection);
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
    validateErrorResponse(res, testDB.getName(), nonexistentUUID, coll.getName(), null);

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection.");
    const coll2 = testDB['coll_2'];
    assert.commandWorked(coll2.insert({x: 1, y: 1}));
    cmdObj[cmd] = coll2.getName();
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, testDB.getName(), uuid, coll2.getName(), coll.getName());

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace does not exist.");
    assert.commandWorked(testDB.runCommand({drop: coll2.getName()}));
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, testDB.getName(), uuid, coll2.getName(), coll.getName());
    assert(!testDB.getCollectionNames().includes(coll2.getName()));

    jsTestLog("The command '" + cmd +
              "' fails with CollectionUUIDMismatch even if the database does not exist.");
    const nonexistentDB = testDB.getSiblingDB(testDB.getName() + '_nonexistent');
    cmdObj[cmd] = 'nonexistent';
    res = assert.commandFailedWithCode(nonexistentDB.runCommand(cmdObj),
                                       ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, nonexistentDB.getName(), uuid, 'nonexistent', null);

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
    validateErrorResponse(res, testDB.getName(), uuid, viewName, coll.getName());
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
    validateErrorResponse(res, testDB.getName(), uuid, tsCollName, coll.getName());
    assert.commandWorked(testDB.runCommand({drop: tsCollName, writeConcern: {w: "majority"}}));

    jsTestLog("Only collections in the same database are specified by actualCollection.");
    const otherDB = testDB.getSiblingDB(testDB.getName() + '_2');
    assert.commandWorked(otherDB.dropDatabase());
    assert.commandWorked(otherDB.dropDatabase());
    const coll3 = otherDB['coll_3'];
    assert.commandWorked(coll3.insert({_id: 2}));
    cmdObj[cmd] = coll3.getName();
    res =
        assert.commandFailedWithCode(otherDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, otherDB.getName(), uuid, coll3.getName(), null);
};

testCommand("aggregate", {aggregate: "", pipeline: [{$indexStats: {}}], cursor: {}});
testCommand("aggregate", {aggregate: "", pipeline: [{$collStats: {latencyStats: {}}}], cursor: {}});
})();
