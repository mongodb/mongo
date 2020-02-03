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

const createIndexAndCRUDInTxn = function(sessionDB, collName, explicitCollCreate) {
    if (explicitCollCreate) {
        assert.commandWorked(sessionDB.runCommand({create: collName}));
    }
    let sessionColl = sessionDB[collName];
    assert.commandWorked(sessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]}));
    assert.commandWorked(sessionColl.insert({a: 1}));
    assert.eq(sessionColl.find({a: 1}).itcount(), 1);
    assert.eq(sessionColl.find({}).itcount(), 1);
};
