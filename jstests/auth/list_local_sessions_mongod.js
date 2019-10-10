/*
 * Auth test for the $listLocalSessions aggregation stage on mongods.
 */
(function() {
'use strict';

load("jstests/auth/list_local_sessions_base.js");

const mongod = MongoRunner.runMongod({auth: ""});
runListLocalSessionsTest(mongod);
MongoRunner.stopMongod(mongod);
})();
