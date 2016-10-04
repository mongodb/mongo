// Tests that MongoD fails to start with the correct error message if mongod.lock exists in the
// dbpath.
(function() {
    "use strict";

    var baseName = "jstests_lock_file_fail_to_open";

    var dbPath = MongoRunner.dataPath + baseName + "/";

    // Start a MongoD just to get a lockfile in place.
    var mongo1 = MongoRunner.runMongod({dbpath: dbPath, waitForConnect: true});

    try {
        clearRawMongoProgramOutput();
        // Start another one which should fail to start as there is already a lockfile in its
        // dbpath.
        var mongo2 = null;
        try {
            // Can't use assert.throws as behavior is different on Windows/Linux.
            mongo2 = MongoRunner.runMongod({dbpath: dbPath, noCleanData: true});
        } catch (ex) {
        }
        // We should have failed to start.
        assert(mongo2 === null);
        assert.soon(() => {
            var logContents = rawMongoProgramOutput();
            return logContents.indexOf("Unable to lock file") > 0 ||
                // Windows error message is different.
                logContents.indexOf("Unable to create/open lock file") > 0;
        });
    } finally {
        MongoRunner.stopMongod(mongo1);
    }

})();
