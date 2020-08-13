/**
 * Tests that mongod fails to start if enableMajorityReadConcern is set to true on storage engines
 * that do not support read concern majority.
 */
(function() {
"use strict";

const storageEngine = jsTest.options().storageEngine;
if (storageEngine === "wiredTiger") {
    return;
}

const conn = MongoRunner.runMongod({enableMajorityReadConcern: true});
assert(!conn);
var logContents = rawMongoProgramOutput();
assert(logContents.indexOf("Cannot initialize " + storageEngine + " with " +
                           "'enableMajorityReadConcern=true' as it does not support" +
                           " read concern majority") > 0);
})();