/*
 * Make sure that listCommands on mongods doesn't require authentication.
 */
(function() {
'use strict';

load("jstests/auth/listcommands_preauth_base.js");

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
})();
