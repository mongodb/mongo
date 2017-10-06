load('jstests/libs/sessions_collection.js');

(function() {
    "use strict";

    var startSession = {startSession: 1};
    var conn = MongoRunner.runMongod({nojournal: ""});

    var admin = conn.getDB("admin");
    var config = conn.getDB("config");

    // Test that we can use sessions before the sessions collection exists.
    {
        validateSessionsCollection(conn, false, false);
        assert.commandWorked(admin.runCommand({startSession: 1}));
        validateSessionsCollection(conn, false, false);
    }

    // Test that a refresh will create the sessions collection.
    {
        assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
        validateSessionsCollection(conn, true, true);
    }

    // Test that a refresh will (re)create the TTL index on the sessions collection.
    {
        assert.commandWorked(config.system.sessions.dropIndex({lastUse: 1}));
        validateSessionsCollection(conn, true, false);
        assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
        validateSessionsCollection(conn, true, true);
    }

})();
