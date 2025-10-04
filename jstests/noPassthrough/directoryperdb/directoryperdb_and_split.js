if (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger") {
    let baseDir = jsTestName();
    let dbpath = MongoRunner.dataPath + baseDir + "/";

    let m = MongoRunner.runMongod({dbpath: dbpath, wiredTigerDirectoryForIndexes: "", directoryperdb: ""});
    const db = m.getDB("foo");
    db.bar.insert({x: 1});
    assert.eq(1, db.bar.count());

    db.adminCommand({fsync: 1});

    assert(listFiles(dbpath + "/foo/index").length > 0);
    assert(listFiles(dbpath + "/foo/collection").length > 0);

    MongoRunner.stopMongod(m);

    // Subsequent attempts to start server using same dbpath but different
    // wiredTigerDirectoryForIndexes and directoryperdb options should fail.
    assert.throws(() => MongoRunner.runMongod({dbpath: dbpath, port: m.port, restart: true}));
    assert.throws(() => MongoRunner.runMongod({dbpath: dbpath, port: m.port, restart: true, directoryperdb: ""}));
    assert.throws(() =>
        MongoRunner.runMongod({dbpath: dbpath, port: m.port, restart: true, wiredTigerDirectoryForIndexes: ""}),
    );
}
