/**
 * Confirm that replication of an invalid index specification causes server abort (where index
 * version is >= 2).
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    const testName = "invalid_index_spec";
    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();

    let primaryDB = replTest.getPrimary().getDB(testName);

    // Set a fail point that allows for index creation with invalid spec fields.
    primaryDB.adminCommand(
        {configureFailPoint: "skipIndexCreateFieldNameValidation", mode: "alwaysOn"});

    clearRawMongoProgramOutput();

    // Create a V1 index with invalid spec field. Expected to replicate without error or server
    // abort.
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: "test", indexes: [{v: 1, name: "w_1", key: {w: 1}, invalidOption1: 1}]}));

    // Create a V2 index with invalid spec field. Expected to cause server abort on replication.
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: "test", indexes: [{v: 2, name: "x_1", key: {x: 1}, invalidOption2: 1}]}));

    const msg1 = "Fatal assertion 16359";
    const msg2 = "InvalidIndexSpecificationOption: The field 'invalidOption2'";

    const assertFn = function() {
        return rawMongoProgramOutput().match(msg1) && rawMongoProgramOutput().match(msg2);
    };
    assert.soon(assertFn, "Replication should have aborted on invalid index specification", 60000);

    replTest.stopSet(undefined,
                     undefined,
                     {allowedExitCodes: [MongoRunner.EXIT_ABRUPT, MongoRunner.EXIT_ABORT]});

})();
