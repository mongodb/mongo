/**
 * Tests that mongod fails to start if enableMajorityReadConcern is set to false on non test only
 * storage engines, which are only expected to support read concern majority.
 *
 * Also verifies that the server automatically uses enableMajorityReadConcern=false if we're using a
 * test only storage engine.
 */
(function() {
"use strict";

const storageEngine = jsTest.options().storageEngine;
if (storageEngine === "wiredTiger" || storageEngine === "inMemory") {
    const conn = MongoRunner.runMongod({enableMajorityReadConcern: false});
    assert(!conn);
    var logContents = rawMongoProgramOutput();
    assert(logContents.indexOf("enableMajorityReadConcern:false is no longer supported") > 0);
    return;
}

if (storageEngine === "ephemeralForTest") {
    const conn = MongoRunner.runMongod();
    assert(conn);
    var logContents = rawMongoProgramOutput();
    assert(
        logContents.indexOf(
            "Test storage engine does not support enableMajorityReadConcern=true, forcibly setting to false") >
        0);
    MongoRunner.stopMongod(conn);
    return;
}
})();