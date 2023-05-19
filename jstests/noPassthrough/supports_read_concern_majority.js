/**
 * Tests that mongod fails to start if enableMajorityReadConcern is set to false.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod({enableMajorityReadConcern: false});
assert(!conn);
const logContents = rawMongoProgramOutput();
assert(logContents.indexOf("enableMajorityReadConcern:false is no longer supported") > 0);
})();
