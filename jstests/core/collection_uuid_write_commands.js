/**
 * Tests the collectionUUID parameter of the insert, update and delete commands.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
 *   tenant_migration_incompatible,
 *   no_selinux,
 * ]
 */
(function() {
'use strict';

var validateErrorResponse = function(res, collectionUUID, actualNamespace) {
    if (res.writeErrors) {
        // Sharded cluster scenario.
        res = res.writeErrors[0];
    }

    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.actualNamespace, actualNamespace);
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
    validateErrorResponse(res, nonexistentUUID, "");

    jsTestLog("The command '" + cmd +
              "' fails when the provided UUID corresponds to a different collection.");
    const coll2 = testDB['coll_2'];
    assert.commandWorked(coll2.insert({_id: 0}));
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
};

testCommand("insert", {insert: "", documents: [{inserted: true}]});
testCommand("update", {update: "", updates: [{q: {_id: 0}, u: {$set: {updated: true}}}]});
testCommand("delete", {delete: "", deletes: [{q: {_id: 0}, limit: 1}]});
})();
