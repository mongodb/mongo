/**
 * Verifies that we log a startup warning whenever 'internalQueryFrameworkControl' is set to any
 * value besides 'forceClassicEngine'.
 */
(function() {
"use strict";

if (TestData.setParameters && TestData.setParameters.internalQueryFrameworkControl) {
    jsTestLog(
        "Skipping test because this variant configures 'internalQueryFrameworkControl' manually");
    quit();
}

const expectedStartupLogId = 9473500;
const nonForceClassicValues = ["trySbeRestricted", "trySbeEngine"];

for (const val of nonForceClassicValues) {
    jsTestLog("Testing starting up a mongod with 'internalQueryFrameworkControl' set to " +
              tojson(val));
    const conn = MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: val}});
    assert.neq(null, conn, 'mongod was unable to start up');
    assert(checkLog.checkContainsOnce(conn, expectedStartupLogId));
    MongoRunner.stopMongod(conn);
}
})();
