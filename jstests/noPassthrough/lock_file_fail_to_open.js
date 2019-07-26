// Tests that MongoD fails to start with the correct error message if mongod.lock exists in the
// dbpath.
(function() {
"use strict";

var baseName = "jstests_lock_file_fail_to_open";

var dbPath = MongoRunner.dataPath + baseName + "/";

// Start a MongoD just to get a lockfile in place.
var mongo1 = MongoRunner.runMongod({dbpath: dbPath, waitForConnect: true});

clearRawMongoProgramOutput();
// Start another one which should fail to start as there is already a lockfile in its
// dbpath.
var mongo2 = null;
mongo2 = MongoRunner.runMongod({dbpath: dbPath, noCleanData: true});
// We should have failed to start.
assert(mongo2 === null);

var logContents = rawMongoProgramOutput();
assert(logContents.indexOf("Unable to lock the lock file") > 0 ||
       // Windows error message is different.
       logContents.indexOf("Unable to create/open the lock file") > 0);

MongoRunner.stopMongod(mongo1);
})();
