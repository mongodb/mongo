/**
 * Helper function shared by createCollection inside txns tests.
 */
const createCollAndCRUDInTxn = function(sessionDB, collName, explicitCreate) {
    if (explicitCreate) {
        assert.commandWorked(sessionDB.runCommand({create: collName}));
    }
    let sessionColl = sessionDB[collName];
    assert.commandWorked(sessionColl.insert({a: 1}));
    assert.eq(sessionColl.find({a: 1}).itcount(), 1);
    assert.eq(sessionColl.find({}).itcount(), 1);
};
