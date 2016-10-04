// Test for quotaFiles off by one file limit issue - SERVER-3420.

if (0) {  // SERVER-3420

    baseName = "jstests_disk_quota2";

    var m = MongoRunner.runMongod({quotaFiles: 2, smallfiles: ""});
    db = m.getDB(baseName);

    big = new Array(10000).toString();

    // Insert documents until quota is exhausted.
    var coll = db[baseName];
    var res = coll.insert({b: big});
    while (!res.hasWriteError()) {
        res = coll.insert({b: big});
    }

    // Trigger allocation of an additional file for a 'special' namespace.
    for (n = 0; !db.getLastError(); ++n) {
        db.createCollection('' + n);
    }

    // Check that new docs are saved in the .0 file.
    for (i = 0; i < n; ++i) {
        c = db['' + i];
        res = c.insert({b: big});
        if (!res.hasWriteError()) {
            var recordId = c.find().showRecord()[0].$recordId;
            assert.eq(0, recordId >> 32);
        }
    }
}
