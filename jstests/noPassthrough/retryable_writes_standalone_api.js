/*
 * Verify behavior of retryable write commands on a standalone merizod.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    const standalone = MerizoRunner.runMerizod();
    const testDB = standalone.getDB("test");

    // Commands sent to standalone nodes are not allowed to have transaction numbers.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {insert: "foo", documents: [{x: 1}], txnNumber: NumberLong(1), lsid: {id: UUID()}}),
        ErrorCodes.IllegalOperation,
        "expected command with transaction number to fail on standalone merizod");

    MerizoRunner.stopMerizod(standalone);
}());
