/**
 * Test that setting profiling level is not allowed when FCV is unset; otherwise
 * system.profile might be created without a UUID.
 */
(function() {
    "use strict";

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    var primaryAdmin = replTest.nodes[0].getDB("admin");
    assert.commandWorked(primaryAdmin.adminCommand(
        {configureFailPoint: "hangBetweenAssigningUUIDsAndSetFCV", mode: "alwaysOn"}));

    // rs.initiate() will assign UUIDs to all the existing collections and set
    // the FCV. And the failpoint makes sure we capture the window between those
    // two operations.
    const rsInitiate = startParallelShell("rs.initiate();", replTest.nodes[0].port);

    // Enabling profiling should fail
    try {
        primaryAdmin.setProfilingLevel(2);
        // Since the function setProfilingLevel() returns the result of assert.commandWorked(), we
        // cannot use assert.commandFailed and need a try-catch block to check if setProfilingLevel
        // fails.
        assert(false);
    } catch (e) {
        // Make sure the exception was caused by setProfilingLevel rather than
        // assert(false).
        assert(
            -1 !==
            e.message.indexOf(
                "profiling level cannot be set when featureCompatibilityVersion is uninitialized"));
    }

    assert.commandWorked(primaryAdmin.adminCommand(
        {configureFailPoint: "hangBetweenAssigningUUIDsAndSetFCV", mode: "off"}));
    const exitCode = rsInitiate();
    assert.eq(0, exitCode, 'expected shell to exit cleanly');
    assert.commandWorked(primaryAdmin.setProfilingLevel(2));

    // Wait for replication and then check if UUID exists
    replTest.awaitReplication();
    let collectionInfos = primaryAdmin.getCollectionInfos({name: "system.profile"});
    assert(collectionInfos[0].info.uuid,
           "Expect uuid for collection: " + tojson(collectionInfos[0]));

    replTest.stopSet();
})();
