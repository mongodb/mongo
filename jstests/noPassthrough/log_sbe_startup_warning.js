/**
 * Verifies that we log a startup warning whenever 'internalQueryForceClassicEngine' is set to
 * 'false'.
 */
(function() {
"use strict";
if (TestData.setParameters && TestData.setParameters.internalQueryForceClassicEngine) {
    jsTestLog(
        "Skipping test because this variant configures 'internalQueryForceClassicEngine' manually");
    quit();
}
const expectedStartupLogId = 9473500;
const conn = MongoRunner.runMongod({setParameter: {internalQueryForceClassicEngine: false}});
assert.neq(null, conn, 'mongod was unable to start up');
assert(checkLog.checkContainsOnce(conn, expectedStartupLogId));
MongoRunner.stopMongod(conn);
})();
