/**
 * Test that old journal files are eventually deleted.
 */

if (db.serverBuildInfo().bits == 32) {
    print("skip on 32 bit systems");
} else {
    var conn = MongoRunner.runMongod({
        journal: "",
        smallfiles: "",
        syncdelay: 5,  // seconds between fsyncs.
    });
    db = conn.getDB("test");

    // Returns true if j._0 exists.
    function firstJournalFileExists() {
        var files = listFiles(conn.dbpath + "/journal");
        for (var i = 0; i < files.length; i++) {
            if (files[i].baseName === "j._0") {
                return true;
            }
        }
        return false;
    }

    // Represents the cummulative total of the number of journal files created.
    function getLatestJournalFileNum() {
        var files = listFiles(conn.dbpath + "/journal");
        var latest = 0;
        files.forEach(function(file) {
            if (file.baseName !== "lsn") {
                var fileNum = NumberInt(file.baseName[file.baseName.length - 1]);
                latest = Math.max(latest, fileNum);
            }
        });
        return latest;
    }

    var stringSize = 1024 * 1024;
    var longString = new Array(stringSize).join("x");

    // Insert some data to create the first journal file.
    var numInserted = 0;
    while (numInserted < 100) {
        db.foo.insert({_id: numInserted++, s: longString});
    }
    assert.soon(firstJournalFileExists, "Should have created a journal file");

    // Do writes until the first journal file is deleted, or we give up waiting.
    var maxJournalFiles = 10;
    while (firstJournalFileExists() && getLatestJournalFileNum() < maxJournalFiles) {
        db.foo.insert({_id: numInserted++, s: longString});

        if (numInserted % 100 == 0) {
            jsTestLog("numInserted: " + numInserted);
            db.adminCommand({fsync: 1});
            db.foo.remove({});
            db.adminCommand({fsync: 1});
        }
    }

    assert(!firstJournalFileExists(), "Expected to have deleted the first journal file by now");
    MongoRunner.stopMongod(conn);
}
