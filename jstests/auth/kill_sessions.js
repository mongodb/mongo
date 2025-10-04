import {KillSessionsTestHelper} from "jstests/libs/kill_sessions.js";

// This test involves killing all sessions, which will not work as expected if the kill command is
// sent with an implicit session.
TestData.disableImplicitSessions = true;

let forExec = MongoRunner.runMongod({auth: ""});
let forKill = new Mongo(forExec.host);
let forVerify = new Mongo(forExec.host);
KillSessionsTestHelper.initializeAuth(forExec);
forVerify.getDB("admin").auth("super", "password");
KillSessionsTestHelper.runAuth(forExec, forKill, [forVerify]);
MongoRunner.stopMongod(forExec);
