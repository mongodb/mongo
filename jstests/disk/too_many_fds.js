// Create more than 1024 files on certain storage engines, then restart the server and see that it
// can still listen on fd's smaller than FD_SETSIZE.

function doTest() {
    var baseName = "jstests_disk_too_many_fds";

    var m = MongoRunner.runMongod({smallfiles: "", nssize: 1});
    // Make 1026 collections, each in a separate database.  On some storage engines, this may cause
    // 1026 files to be created.
    for (var i = 1; i < 1026; ++i) {
        var db = m.getDB("db" + i);
        var coll = db.getCollection("coll" + i);
        assert.writeOK(coll.insert({}));
    }

    MongoRunner.stopMongod(m);

    // Ensure we can still start up with that many files.
    var m2 = MongoRunner.runMongod(
        {dbpath: m.dbpath, smallfiles: "", nssize: 1, restart: true, cleanData: false});
    assert.eq(1, m2.getDB("db1025").getCollection("coll1025").count());
}

if (db.serverBuildInfo().bits == 64) {
    doTest();
} else {
    print("Skipping.  Only run this test on 64bit builds");
}
