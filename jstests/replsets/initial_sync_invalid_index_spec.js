/**
 * Confirm that replica members undergoing initial sync fail if an invalid index specification is
 * encountered (where index version is >= 2).
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    const testName = "initial_sync_invalid_index_spec";
    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    let primaryDB = replTest.getPrimary().getDB(testName);

    // Create a V2 index with invalid spec field.
    primaryDB.adminCommand(
        {configureFailPoint: "skipIndexCreateFieldNameValidation", mode: "alwaysOn"});
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: "test", indexes: [{v: 2, name: "x_1", key: {x: 1}, invalidOption: 1}]}));

    // Add another node to the replica set to allow an initial sync to occur.
    replTest.add();

    clearRawMongoProgramOutput();
    reInitiateWithoutThrowingOnAbortedMember(replTest);

    const msgInvalidOption = "The field 'invalidOption' is not valid for an index specification";
    const msgInitialSyncFatalAssertion = "Fatal assertion 40088 InitialSyncFailure";

    // As part of the initsync-3dot2-rhel-62 evergreen variant, we run this test with a setParameter
    // of "use3dot2InitialSync=true" which exercises 3.2 initial sync behavior. This path will
    // trigger a different fatal assertion than the normal 3.4 path, which we need to handle here.
    // TODO: Remove this assertion check when the 'use3dot2InitialSync' setParameter is retired.
    const msg3dot2InitialSyncFatalAssertion = "Fatal Assertion 16233";

    const assertFn = function() {
        return rawMongoProgramOutput().match(msgInvalidOption) &&
            (rawMongoProgramOutput().match(msgInitialSyncFatalAssertion) ||
             rawMongoProgramOutput().match(msg3dot2InitialSyncFatalAssertion));
    };
    assert.soon(assertFn, "Initial sync should have aborted on invalid index specification");

    replTest.stopSet(undefined,
                     undefined,
                     {allowedExitCodes: [MongoRunner.EXIT_ABRUPT, MongoRunner.EXIT_ABORT]});

})();
