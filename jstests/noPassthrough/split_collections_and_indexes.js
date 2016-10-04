if (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger") {
    var baseDir = "jstests_split_c_and_i";

    var dbpath = MongoRunner.dataPath + baseDir + "/";

    var m = MongoRunner.runMongod({dbpath: dbpath, wiredTigerDirectoryForIndexes: ''});
    db = m.getDB("foo");
    db.bar.insert({x: 1});
    assert.eq(1, db.bar.count());

    db.adminCommand({fsync: 1});

    assert(listFiles(dbpath + "/index").length > 0);
    assert(listFiles(dbpath + "/collection").length > 0);

    MongoRunner.stopMongod(m.port);

    // Subsequent attempts to start server using same dbpath but different
    // wiredTigerDirectoryForIndexes and directoryperdb options should fail.
    assert.isnull(MongoRunner.runMongod({dbpath: dbpath, restart: true}));
    assert.isnull(MongoRunner.runMongod({dbpath: dbpath, restart: true, directoryperdb: ''}));
    assert.isnull(MongoRunner.runMongod(
        {dbpath: dbpath, restart: true, wiredTigerDirectoryForIndexes: '', directoryperdb: ''}));
}
