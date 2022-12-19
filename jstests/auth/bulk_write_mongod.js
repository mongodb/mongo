/*
 * Auth test for the bulkWrite command on mongods.
 */
(function() {
'use strict';

load("jstests/auth/lib/bulk_write_base.js");

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);
})();
