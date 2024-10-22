/**
 * Tests that errors generated as part of ReplicationCoordinatorImpl startup do not cause the server
 * to hang during shutdown.
 *
 * @tags: [requires_persistence, requires_fcv_60]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

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

jsTestLog("Done initiating set. Stopping node.");

rst.stop(0);

clearRawMongoProgramOutput();

// We set the fail point to make the node encounter a fatal error while trying to load its on-disk
// config. This code does not run before the restart because there is no config on disk yet.
jsTestLog("Restarting node. It should fassert.");
const node = rst.restart(0, {startClean: false, waitForConnect: false});

const exitCode = waitProgram(node.pid);
assert.eq(exitCode, MongoRunner.EXIT_ABRUPT);

assert.soon(
    function() {
        return rawMongoProgramOutput("Fatal assertion").search(/6111701/) >= 0;
    },
    "Node should have fasserted upon encountering a fatal error during startup",
    ReplSetTest.kDefaultTimeoutMS);