/**
 * Helper function shared by createIndexes inside txns tests.
 */

const indexSpecs = {
    key: {a: 1},
    name: "a_1"
};
const conflictingIndexSpecs = {
    key: {a: -1},
    name: "a_1"
};

const createIndexAndCRUDInTxn = function(sessionDB, collName, explicitCollCreate, multikeyIndex) {
    if (undefined === explicitCollCreate) {
        doassert('createIndexAndCRUDInTxn called with undefined explicitCollCreate');
    }
    if (undefined === multikeyIndex) {
        doassert('createIndexAndCRUDInTxn called with undefined multikeyIndex');
    }
    if (explicitCollCreate) {
        assert.commandWorked(sessionDB.runCommand({create: collName}));
    }
    let sessionColl = sessionDB[collName];
    assert.commandWorked(sessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]}));
    assert.commandWorked(sessionColl.createIndex({_id: 1}));
    if (multikeyIndex) {
        assert.commandWorked(sessionColl.insert({a: [1, 2, 3]}));
    } else {
        assert.commandWorked(sessionColl.insert({a: 1}));
    }
    assert.eq(sessionColl.find({a: 1}).itcount(), 1);
    assert.commandWorked(sessionColl.insert({_id: 1}));
    assert.commandWorked(sessionColl.deleteOne({_id: 1}));
    assert.eq(sessionColl.find({}).itcount(), 1);
};
