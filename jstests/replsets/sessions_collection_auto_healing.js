load('jstests/libs/sessions_collection.js');

(function() {
    "use strict";

    var replTest = new ReplSetTest({name: 'refresh', nodes: 3});
    var nodes = replTest.startSet();

    replTest.initiate();
    var primary = replTest.getPrimary();
    var primaryAdmin = primary.getDB("admin");

    replTest.awaitSecondaryNodes();
    var secondary = replTest.liveNodes.slaves[0];
    var secondaryAdmin = secondary.getDB("admin");

    // Test that we can use sessions on the primary
    // before the sessions collection exists.
    {
        validateSessionsCollection(primary, false, false);

        assert.commandWorked(primaryAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(primary, false, false);
    }

    // Test that we can use sessions on secondaries
    // before the sessions collection exists.
    {
        validateSessionsCollection(primary, false, false);
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(secondaryAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(primary, false, false);
        validateSessionsCollection(secondary, false, false);
    }

    // Test that a refresh on a secondary does not create the sessions
    // collection, on either the secondary or the primary.
    {
        validateSessionsCollection(primary, false, false);
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, false, false);
        validateSessionsCollection(secondary, false, false);
    }

    // Test that a refresh on the primary creates the sessions collection.
    {
        validateSessionsCollection(primary, false, false);
        validateSessionsCollection(secondary, false, false);

        assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);
    }

    // Test that a refresh on a secondary will not create the
    // TTL index on the sessions collection.
    {
        assert.commandWorked(primary.getDB("config").system.sessions.dropIndex({lastUse: 1}));

        validateSessionsCollection(primary, true, false);

        assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, false);
    }

    // Test that a refresh on the primary will create the
    // TTL index on the sessions collection.
    {
        validateSessionsCollection(primary, true, false);

        assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(primary, true, true);
    }

    replTest.stopSet();
})();
