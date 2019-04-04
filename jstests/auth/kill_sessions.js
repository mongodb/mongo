load("jstests/libs/kill_sessions.js");

(function() {
    'use strict';

    // TODO SERVER-35447: This test involves killing all sessions, which will not work as expected
    // if the kill command is sent with an implicit session.
    TestData.disableImplicitSessions = true;

    var forExec = MerizoRunner.runMerizod({auth: ""});
    var forKill = new Merizo(forExec.host);
    var forVerify = new Merizo(forExec.host);
    KillSessionsTestHelper.initializeAuth(forExec);
    forVerify.getDB("admin").auth("super", "password");
    KillSessionsTestHelper.runAuth(forExec, forKill, [forVerify]);
    MerizoRunner.stopMerizod(forExec);
})();
