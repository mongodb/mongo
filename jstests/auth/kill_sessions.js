load("jstests/libs/kill_sessions.js");

(function() {
    'use strict';

    var forExec = MongoRunner.runMongod({nojournal: "", auth: ""});
    var forKill = new Mongo(forExec.host);
    var forVerify = new Mongo(forExec.host);
    KillSessionsTestHelper.initializeAuth(forExec);
    forVerify.getDB("admin").auth("super", "password");
    KillSessionsTestHelper.runAuth(forExec, forKill, [forVerify]);
    MongoRunner.stopMongod(forExec);
})();
