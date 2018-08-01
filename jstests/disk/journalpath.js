/**
 * Confirms that journal files are stored in the expected directory when using "--journalPath" or
 * the default directory when no journalPath is passed.
 * @tags: [requires_wiredtiger]
 */
(function() {
    "use strict";

    const baseDir = 'jstests_disk_journalpath';
    const journalSubDir = 'journal_test';
    const dbpath = MongoRunner.dataPath + baseDir + '/';
    const storageEngine = db.serverStatus().storageEngine.name;

    // Matches wiredTiger journal files (WiredTigerLog or WiredTigerPreplog)
    const journalFileMatcher = /WiredTiger(Preplog|Log)\..+$/;

    /**
     * Returns the current connection after ensuring journal path directory 'dir' contains only
     * journal files.
     */
    let checkFilesInJournalDirectory = function(conn, dir) {
        assert(conn, "Mongod failed to start up.");
        let files = listFiles(dir);
        let fileCount = 0;
        for (let f in files) {
            assert(!files[f].isDirectory, 'Unexpected directory found in journal path.');
            fileCount += 1;
            assert(journalFileMatcher.test(files[f].name),
                   'In directory:' + dir + ' found unexpected file: ' + files[f].name);
        }
        assert(fileCount > 0, 'Expected more than zero nondirectory files in database directory');
    };

    // Ensure journal files are found in correct default location.
    let mongoServer = MongoRunner.runMongod({storageEngine: storageEngine, dbpath: dbpath});
    checkFilesInJournalDirectory(mongoServer, dbpath + "journal");
    MongoRunner.stopMongod(mongoServer);

    // Check journal files location if a custom journal path directory is passed.
    mongoServer = MongoRunner.runMongod(
        {storageEngine: storageEngine, dbpath: dbpath, journalPath: dbpath + journalSubDir});
    checkFilesInJournalDirectory(mongoServer, dbpath + journalSubDir);
    MongoRunner.stopMongod(mongoServer);
})();
