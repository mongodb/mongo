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

    const msg1 = "The field 'invalidOption' is not valid for an index specification";
    const msg2 = "Fatal assertion 40088 InitialSyncFailure";

    const assertFn = function() {
        return rawMongoProgramOutput().match(msg1) && rawMongoProgramOutput().match(msg2);
    };
    assert.soon(assertFn, "Initial sync should have aborted on invalid index specification", 60000);

    replTest.stopSet(undefined,
                     undefined,
                     {allowedExitCodes: [MongoRunner.EXIT_ABRUPT, MongoRunner.EXIT_ABORT]});

})();
