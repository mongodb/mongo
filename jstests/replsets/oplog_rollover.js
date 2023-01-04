/**
 * Test that oplog (on both primary and secondary) rolls over when its size exceeds the configured
 * maximum. This test runs on wiredTiger storage engine and inMemory storage engine (if available).
 */
(function() {
"use strict";

load("jstests/replsets/libs/oplog_rollover_test.js");

oplogRolloverTest("wiredTiger");

if (jsTest.options().storageEngine !== "inMemory") {
    jsTestLog(
        "Skipping inMemory test because inMemory storageEngine was not compiled into the server.");
    return;
}

oplogRolloverTest("inMemory");
})();
