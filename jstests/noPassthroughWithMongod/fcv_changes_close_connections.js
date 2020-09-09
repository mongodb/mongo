// Test closing connections from internal clients on featureCompatibilityVersion changes.
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const adminDB = db.getSiblingDB("admin");
const testDB = db.getSiblingDB("test");
const collName = "coll";

checkFCV(adminDB, latestFCV);
assert.commandWorked(testDB.runCommand({insert: collName, documents: [{x: 1}]}));

function testFCVChange({fcvDoc, expectConnectionClosed = true} = {}) {
    let findCmdFailPoint = configureFailPoint(
        db, "waitInFindBeforeMakingBatch", {nss: "test.coll", shouldCheckForInterrupt: true});

    // Mimic running a find command from an internal client with a lower binary version.
    function findWithLowerBinaryInternalClient(expectConnectionClosed) {
        const testDB = db.getSiblingDB("test");

        // The value of the 'internalClient.maxWireVersion' does not matter for the purpose of this
        // test as long as it is less than the latest server's maxWireVersion. The server will tag
        // this connection as an internal client connection with a lower binary version on receiving
        // a 'hello' with 'internalClient.maxWireVersion' less than the server's maxWireVersion.
        // Connections with such a tag are closed by the server on certain FCV changes.
        //
        // The 'hello' command below will succeed because this is run through the shell and the
        // shell is always compatible talking to the server. In reality though, a real internal
        // client with a lower binary version is expected to hang up immediately after receiving the
        // response to the 'hello' command from a latest server with an upgraded FCV.
        assert.commandWorked(testDB.adminCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(9), maxWireVersion: NumberInt(9)}
        }));

        // Since mongod expects other cluster members to always include explicit read/write concern
        // (on commands that accept read/write concern), this test must be careful to mimic this
        // behavior.
        if (expectConnectionClosed) {
            const e = assert.throws(
                () => testDB.runCommand({find: "coll", readConcern: {level: "local"}}));
            assert.includes(e.toString(), "network error while attempting to run command");
        } else {
            assert.commandWorked(testDB.runCommand({find: "coll", readConcern: {level: "local"}}));
        }
    }

    const joinFindThread = startParallelShell(
        funWithArgs(findWithLowerBinaryInternalClient, expectConnectionClosed), db.getMongo().port);

    jsTestLog("Waiting for waitInFindBeforeMakingBatch failpoint");
    findCmdFailPoint.wait();

    jsTestLog("Updating featureCompatibilityVersion document to: " + tojson(fcvDoc));
    assert.commandWorked(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"}, fcvDoc));

    jsTestLog("Turning off waitInFindBeforeMakingBatch failpoint");
    findCmdFailPoint.off();

    jsTestLog("Joining findThread");
    joinFindThread();
}

function runTest(oldVersion) {
    jsTestLog("Testing with oldVersion: " + oldVersion);

    // Downgrading to oldVersion.
    // Test that connections from internal clients with lower binary versions are closed on FCV
    // downgrade. In reality, there shouldn't be any long-lived open connections from internal
    // clients with lower binary versions before the downgrade, because they are expected to hang up
    // immediately after receiving the response to the initial 'hello' command from an incompatible
    // server. But we still want to test that the server also try to close those incompatible
    // connections proactively on FCV downgrade.
    testFCVChange({
        fcvDoc: {version: oldVersion, targetVersion: oldVersion, previousVersion: latestFCV},
    });

    // Downgraded to oldVersion.
    // Test that connections from internal clients with lower binary versions are not closed on FCV
    // fully downgraded to oldVersion.
    testFCVChange({fcvDoc: {version: oldVersion}, expectConnectionClosed: false});

    // Upgrading from oldVersion to 'latest'.
    // Test that connections from internal clients with lower binary versions are closed on FCV
    // upgrade.
    testFCVChange({fcvDoc: {version: oldVersion, targetVersion: latestFCV}});

    // Upgraded to 'latest'.
    // Test that connections from internal clients with lower binary versions are closed on FCV
    // fully upgraded to 'latest'.
    testFCVChange({fcvDoc: {version: latestFCV}});
}

// Test upgrade/downgrade between 'latest' and 'last-continuous' if 'last-continuous' is not
// 'last-lts'.
if (lastContinuousFCV !== lastLTSFCV) {
    runTest(lastContinuousFCV);

    // Upgrading from last-lts to last-continuous. This FCV transition is allowed through the
    // setFeatureCompatibilityVersion command with fromConfigServer: true.
    testFCVChange({fcvDoc: {version: lastLTSFCV, targetVersion: lastContinuousFCV}});
}

// Test upgrade/downgrade between 'latest' and 'last-lts'.
runTest(lastLTSFCV);
})();
