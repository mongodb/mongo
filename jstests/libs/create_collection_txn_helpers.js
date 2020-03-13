/**
 * Helper function shared by createCollection inside txns tests.
 */
const createCollAndCRUDInTxn = function(sessionDB, collName, explicitCreate, upsert) {
    if (undefined === explicitCreate) {
        doassert('createCollAndCRUDInTxn called with undefined explicitCreate');
    }
    if (undefined === upsert) {
        doassert('createCollAndCRUDInTxn called with undefined upsert');
    }
    if (explicitCreate) {
        assert.commandWorked(sessionDB.runCommand({create: collName}));
    }
    let sessionColl = sessionDB[collName];
    if (upsert) {
        assert.commandWorked(sessionColl.update({_id: 1}, {$inc: {a: 1}}, {upsert: true}));
    } else {
        assert.commandWorked(sessionColl.insert({a: 1}));
    }
    assert.eq(sessionColl.find({a: 1}).itcount(), 1);
    assert.commandWorked(sessionColl.insert({_id: 2}));
    let resDoc = sessionColl.findAndModify({query: {_id: 2}, update: {$inc: {a: 1}}});
    assert.eq(resDoc._id, 2);
    assert.commandWorked(sessionColl.update({_id: 2}, {$inc: {a: 1}}));
    assert.commandWorked(sessionColl.deleteOne({_id: 2}));
    assert.eq(sessionColl.find({}).itcount(), 1);
};

const assertCollCreateFailedWithCode = function(sessionDB, collName, explicitCreate, upsert, code) {
    if (undefined === explicitCreate) {
        doassert('assertWriteConflictForCollCreate called with undefined explicitCreate');
    }
    if (undefined === upsert) {
        doassert('assertWriteConflictForCollCreate called with undefined upsert');
    }
    if (undefined === code) {
        doassert('assertWriteConflictForCollCreate called with undefined code');
    }
    let sessionColl = sessionDB[collName];
    if (explicitCreate) {
        assert.commandFailedWithCode(sessionDB.createCollection(collName), code);
    } else if (upsert) {
        assert.commandFailedWithCode(sessionColl.update({_id: 1}, {$inc: {a: 1}}, {upsert: true}),
                                     code);
    } else {
        assert.commandFailedWithCode(sessionColl.insert({a: 1}), code);
    }
};
