// Tests that the namespace being watched cannot be a system namespace.
// Mark as assumes_read_preference_unchanged since reading from the non-replicated "system.profile"
// collection results in a failure in the secondary reads suite.
// @tags: [assumes_read_preference_unchanged]
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");     // For 'FixtureHelpers'.
    load("jstests/libs/change_stream_util.js");  // For assert[Valid|Invalid]ChangeStreamNss.

    // Test that a change stream cannot be opened on the "admin", "config", or "local" databases.
    assertInvalidChangeStreamNss("admin");
    assertInvalidChangeStreamNss("config");
    // Not allowed to access 'local' database through mongos.
    if (!FixtureHelpers.isMongos()) {
        assertInvalidChangeStreamNss("local");
    }

    // Test that a change stream cannot be opened on 'system.' collections.
    assertInvalidChangeStreamNss("test", "system.users");
    assertInvalidChangeStreamNss("test", "system.profile");
    assertInvalidChangeStreamNss("test", "system.version");

    // Test that a change stream can be opened on namespaces with 'system' in the name, but not
    // considered an internal 'system dot' namespace.
    assertValidChangeStreamNss("test", "systemindexes");
    assertValidChangeStreamNss("test", "system_users");
}());
