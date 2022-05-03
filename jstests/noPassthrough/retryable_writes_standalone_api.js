/*
 * Verify behavior of retryable write commands on a standalone mongod.
 */
(function() {
"use strict";

const standalone = MongoRunner.runMongod();
const testDB = standalone.getDB("test");

// Commands sent to standalone nodes are not allowed to have transaction numbers.
assert.commandFailedWithCode(
    testDB.runCommand(
        {insert: "foo", documents: [{x: 1}], txnNumber: NumberLong(1), lsid: {id: UUID()}}),
    ErrorCodes.IllegalOperation,
    "expected command with transaction number to fail on standalone mongod");

MongoRunner.stopMongod(standalone);
}());
