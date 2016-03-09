/**
 * Tests that a mongod started with --directoryperdb will write data for database x into a direcotry
 * named x inside the dbpath.
 *
 * This test does not make sense for in-memory storage engines, since they will not produce any data
 * files.
 * @tags: [requires_persistence]
 */

(function() {
    'use strict';

    var baseDir = "jstests_directoryperdb";
    var dbpath = MongoRunner.dataPath + baseDir + "/";

    var isDirectoryPerDBSupported = jsTest.options().storageEngine == "mmapv1" ||
        jsTest.options().storageEngine == "wiredTiger" || !jsTest.options().storageEngine;

    var m = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: ''});

    if (!isDirectoryPerDBSupported) {
        assert.isnull(m, 'storage engine without directoryperdb support should fail to start up');
        return;
    } else {
        assert(m, 'storage engine with directoryperdb support failed to start up');
    }

    var db = m.getDB("foo");
    db.bar.insert({x: 1});
    assert.eq(1, db.bar.count());

    db.adminCommand({fsync: 1});
    var dbpathFiles = listFiles(dbpath);
    var files = dbpathFiles.filter(function(z) {
        return z.name.endsWith("/foo");
    });
    assert.eq(1, files.length, 'dbpath does not contain "foo" directory: ' + tojson(dbpathFiles));

    files = listFiles(files[0].name);
    assert(files.length > 0);

    MongoRunner.stopMongod(m.port);

    // Subsequent attempt to start server using same dbpath without directoryperdb should fail.
    assert.isnull(MongoRunner.runMongod({dbpath: dbpath, restart: true}));
}());
