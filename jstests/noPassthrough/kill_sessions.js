load("jstests/libs/kill_sessions.js");

(function() {
    'use strict';

    // TODO SERVER-35447: This test involves killing all sessions, which will not work as expected
    // if the kill command is sent with an implicit session.
    TestData.disableImplicitSessions = true;

    var conn = MongoRunner.runMongod({nojournal: ""});
    KillSessionsTestHelper.runNoAuth(conn, conn, [conn]);
    MongoRunner.stopMongod(conn);
})();
