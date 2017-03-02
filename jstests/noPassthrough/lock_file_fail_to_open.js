// Tests that BongoD fails to start with the correct error message if bongod.lock exists in the
// dbpath.
(function() {
    "use strict";

    var baseName = "jstests_lock_file_fail_to_open";

    var dbPath = BongoRunner.dataPath + baseName + "/";

    // Start a BongoD just to get a lockfile in place.
    var bongo1 = BongoRunner.runBongod({dbpath: dbPath, waitForConnect: true});

    try {
        clearRawBongoProgramOutput();
        // Start another one which should fail to start as there is already a lockfile in its
        // dbpath.
        var bongo2 = null;
        try {
            // Can't use assert.throws as behavior is different on Windows/Linux.
            bongo2 = BongoRunner.runBongod({dbpath: dbPath, noCleanData: true});
        } catch (ex) {
        }
        // We should have failed to start.
        assert(bongo2 === null);
        assert.soon(() => {
            var logContents = rawBongoProgramOutput();
            return logContents.indexOf("Unable to lock file") > 0 ||
                // Windows error message is different.
                logContents.indexOf("Unable to create/open lock file") > 0;
        });
    } finally {
        BongoRunner.stopBongod(bongo1);
    }

})();
