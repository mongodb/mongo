/**
 * Tests that errors generated as part of ReplicationCoordinatorImpl startup do not cause the server
 * to hang during shutdown.
 *
 * @tags: [requires_persistence, requires_fcv_60]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{
        setParameter: {
            "failpoint.throwBeforeRecoveringTenantMigrationAccessBlockers":
                tojson({mode: "alwaysOn"})
        }
    }],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

jsTestLog("Done initiating set. Restarting.");

const exitCode = MongoRunner.EXIT_ABRUPT;
let exceptionThrown = false;
try {
    rst.restart(0, {
        startClean: false,
    });
} catch (e) {
    assert(e.message.includes("MongoDB process stopped with exit code: " + exitCode),
           () => tojson(e));
    exceptionThrown = true;
}

assert.soon(
    function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*6111701/) >= 0;
    },
    "Node should have fasserted upon encountering a fatal error during startup",
    ReplSetTest.kDefaultTimeoutMS);

assert(exceptionThrown, "Expected restart to fail.");
})();
