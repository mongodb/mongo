/**
 * Tests the collectionUUID parameter of the insert, update and delete commands.
 *
 * @tags: [
 *   requires_fcv_60,
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
'use strict';

const validateErrorResponse = function(
    res, db, collectionUUID, expectedCollection, actualCollection) {
    if (res.writeErrors) {
        // Sharded cluster scenario.
        res = res.writeErrors[0];
    }

    assert.eq(res.db, db);
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, expectedCollection);
    assert.eq(res.actualCollection, actualCollection);
};

var testCommand = function(cmd, cmdObj) {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB['coll'];
    assert.commandWorked(coll.insert({_id: 0}));
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
    assert.commandWorked(coll2.insert({_id: 0}));
    cmdObj[cmd] = coll2.getName();
    cmdObj["collectionUUID"] = uuid;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, testDB.getName(), uuid, coll2.getName(), coll.getName());

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection, even if the " +
              "provided namespace does not exist.");
    assert.commandWorkedOrFailedWithCode(testDB.runCommand({drop: coll2.getName()}),
                                         ErrorCodes.NamespaceNotFound);
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

    jsTestLog("Only collections in the same database are specified by actualCollection.");
    const otherDB = testDB.getSiblingDB(testDB.getName() + '_2');
    assert.commandWorked(otherDB.dropDatabase());
    const coll3 = otherDB['coll_3'];
    assert.commandWorked(coll3.insert({_id: 2}));
    cmdObj[cmd] = coll3.getName();
    res =
        assert.commandFailedWithCode(otherDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, otherDB.getName(), uuid, coll3.getName(), null);
};

testCommand("insert", {insert: "", documents: [{inserted: true}]});
testCommand("update", {update: "", updates: [{q: {_id: 0}, u: {$set: {updated: true}}}]});
testCommand("update",
            {update: "", updates: [{q: {_id: 0}, u: {$set: {updated: true}}, upsert: true}]});
testCommand("delete", {delete: "", deletes: [{q: {_id: 0}, limit: 1}]});
})();
