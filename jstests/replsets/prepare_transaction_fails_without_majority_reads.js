/**
 * Tests that the 'prepareTransaction' command fails against a replica set primary that has
 * 'enableMajorityReadConcern' disabled.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1, nodeOptions: {enableMajorityReadConcern: "false"}});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "prepare_transaction_fails_without_majority_reads";

    const primary = rst.getPrimary();
    const testDB = primary.getDB(dbName);

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 42}));

    assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                                 ErrorCodes.ReadConcernMajorityNotEnabled);

    rst.stopSet();
})();
