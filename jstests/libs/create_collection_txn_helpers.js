/**
 * Helper function shared by createCollection inside txns tests.
 */
const createCollAndCRUDInTxn = function(sessionDB, collName, command, explicitCreate) {
    if (undefined === explicitCreate) {
        doassert('createCollAndCRUDInTxn called with undefined explicitCreate');
    }
    if (explicitCreate) {
        assert.commandWorked(sessionDB.runCommand({create: collName}));
    }
    let sessionColl = sessionDB[collName];
    if (command === "insert") {
        assert.commandWorked(sessionColl.insert({a: 1}));
    } else if (command === "update") {
        assert.commandWorked(sessionColl.update({_id: 1}, {$inc: {a: 1}}, {upsert: true}));
    } else if (command === "findAndModify") {
        assert.commandWorked(sessionDB.runCommand(
            {findAndModify: collName, query: {_id: 1}, update: {$inc: {a: 1}}, upsert: true}));
    } else {
        doassert("createCollAndCRUDInTxn called with invalid command. " +
                 "Must be 'insert', 'update', or 'findAndModify'.");
    }
    assert.eq(sessionColl.find({a: 1}).itcount(), 1);
    assert.commandWorked(sessionColl.insert({_id: 2}));
    let res =
        sessionDB.runCommand({findAndModify: collName, query: {_id: 2}, update: {$inc: {a: 1}}});
    assert.commandWorked(res);
    assert.eq(res.value._id, 2);
    assert.commandWorked(sessionColl.update({_id: 2}, {$inc: {a: 1}}));
    assert.commandWorked(sessionColl.deleteOne({_id: 2}));
    assert.eq(sessionColl.find({}).itcount(), 1);
};

const assertCollCreateFailedWithCode = function(
    sessionDB, collName, command, explicitCreate, code) {
    if (undefined === explicitCreate) {
        doassert('assertWriteConflictForCollCreate called with undefined explicitCreate');
    }
    if (undefined === code) {
        doassert('assertWriteConflictForCollCreate called with undefined code');
    }
    let sessionColl = sessionDB[collName];
    if (explicitCreate) {
        assert.commandFailedWithCode(sessionDB.createCollection(collName), code);
    } else if (command === "insert") {
        assert.commandFailedWithCode(sessionColl.insert({a: 1}), code);
    } else if (command === "update") {
        assert.commandFailedWithCode(sessionColl.update({_id: 1}, {$inc: {a: 1}}, {upsert: true}),
                                     code);
    } else if (command === "findAndModify") {
        assert.commandFailedWithCode(
            sessionDB.runCommand(
                {findAndModify: collName, query: {_id: 1}, update: {$inc: {a: 1}}, upsert: true}),
            code);
    } else {
        doassert("assertCollCreateFailedWithCode called with invalid command. " +
                 "Must be 'insert', 'update', or 'findAndModify'.");
    }
};
