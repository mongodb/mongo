import {KillSessionsTestHelper} from "jstests/libs/kill_sessions.js";

// This test involves killing all sessions, which will not work as expected if the kill command is
// sent with an implicit session.
TestData.disableImplicitSessions = true;

let conn = MongoRunner.runMongod();
KillSessionsTestHelper.runNoAuth(conn, conn, [conn]);
MongoRunner.stopMongod(conn);
