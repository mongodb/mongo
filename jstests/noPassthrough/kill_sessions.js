load("jstests/libs/kill_sessions.js");

(function() {
    'use strict';

    var conn = MongoRunner.runMongod({nojournal: ""});
    KillSessionsTestHelper.runNoAuth(conn, conn, [conn]);
    MongoRunner.stopMongod(conn);
})();
