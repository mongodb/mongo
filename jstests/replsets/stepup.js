// Tests the replSetStepUp command.

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    var name = "stepup";
    var rst = new ReplSetTest({name: name, nodes: 2});

    rst.startSet();
    rst.initiate();
    rst.awaitReplication();

    var primary = rst.getPrimary();
    var secondary = rst.getSecondary();

    const initialSecondaryStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));

    // Step up the primary. Return OK because it's already the primary.
    var res = primary.adminCommand({replSetStepUp: 1});
    assert.commandWorked(res);
    assert.eq(primary, rst.getPrimary());

    // Step up the secondary, but it's not eligible to be primary.
    // Enable fail point on secondary.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));

    assert.writeOK(primary.getDB("test").bar.insert({x: 2}, {writeConcern: {w: 1}}));
    res = secondary.adminCommand({replSetStepUp: 1});
    assert.commandFailedWithCode(res, ErrorCodes.CommandFailed);
    assert.commandWorked(
        secondary.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));

    // Wait for the secondary to catch up by replicating a doc to both nodes.
    assert.writeOK(primary.getDB("test").bar.insert({x: 3}, {writeConcern: {w: "majority"}}));

    // Step up the secondary. Retry since the old primary may step down when we try to ask for its
    // vote.
    assert.soonNoExcept(function() {
        return secondary.adminCommand({replSetStepUp: 1}).ok;
    });

    // Make sure the step up succeeded.
    assert.eq(secondary, rst.getPrimary());

    // Verifies that the given election reason counter is incremented in the way we expect.
    function verifyServerStatusChange(initialStatus, newStatus, fieldName, expectedIncrement) {
        assert.eq(
            initialStatus.electionMetrics[fieldName]["called"] + expectedIncrement,
            newStatus.electionMetrics[fieldName]["called"],
            "expected the 'called' field of " + fieldName + " to increase by " + expectedIncrement);
    }

    const newSecondaryStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));

    // Check that the 'called' field of stepUpCmd has been incremented in serverStatus, and that it
    // has not been incremented in any of the other election reason counters.
    verifyServerStatusChange(initialSecondaryStatus, newSecondaryStatus, "stepUpCmd", 1);
    verifyServerStatusChange(initialSecondaryStatus, newSecondaryStatus, "priorityTakeover", 0);
    verifyServerStatusChange(initialSecondaryStatus, newSecondaryStatus, "catchUpTakeover", 0);
    verifyServerStatusChange(initialSecondaryStatus, newSecondaryStatus, "electionTimeout", 0);
    verifyServerStatusChange(initialSecondaryStatus, newSecondaryStatus, "freezeTimeout", 0);

    rst.stopSet();
})();
