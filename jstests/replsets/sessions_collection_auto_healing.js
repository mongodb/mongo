load('jstests/libs/sessions_collection.js');

(function() {
    "use strict";

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    var replTest = new ReplSetTest({
        name: 'refresh',
        nodes: [{rsConfig: {votes: 1, priority: 1}}, {rsConfig: {votes: 0, priority: 0}}]
    });
    var nodes = replTest.startSet();

    replTest.initiate();
    var primary = replTest.getPrimary();
    var primaryAdmin = primary.getDB("admin");

    replTest.awaitSecondaryNodes();
    var secondary = replTest.getSecondary();
    var secondaryAdmin = secondary.getDB("admin");

    // Test that we can use sessions on the primary before the sessions collection exists.
    {
        validateSessionsCollection(primary, false, false);

        assert.commandWorked(primaryAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(primary, false, false);
    }

    // Test that we can use sessions on secondaries before the sessions collection exists.
    {
        validateSessionsCollection(primary, false, false);

        replTest.awaitReplication();
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(secondaryAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(primary, false, false);

        replTest.awaitReplication();
        validateSessionsCollection(secondary, false, false);
    }

    // Test that a refresh on a secondary creates the sessions collection.
    {
        validateSessionsCollection(primary, false, false);

        replTest.awaitReplication();
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);

        replTest.awaitReplication();
        validateSessionsCollection(secondary, true, true);
    }
    // Test that a refresh on the primary creates the sessions collection.
    {
        assert.commandWorked(primary.getDB("config").runCommand(
            {drop: "system.sessions", writeConcern: {w: "majority"}}));
        validateSessionsCollection(primary, false, false);

        replTest.awaitReplication();
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);
    }

    // Test that a refresh on a secondary will create the TTL index on the sessions collection.
    {
        assert.commandWorked(primary.getDB("config").system.sessions.dropIndex({lastUse: 1}));

        validateSessionsCollection(primary, true, false);

        assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);
    }

    // Test that a refresh on the primary will create the TTL index on the sessions collection.
    {
        assert.commandWorked(primary.getDB("config").system.sessions.dropIndex({lastUse: 1}));

        validateSessionsCollection(primary, true, false);

        assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);
    }

    replTest.stopSet();
})();
