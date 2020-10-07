/**
 * Tests that a mongod started with --directoryperdb will write data for database x into a directory
 * named x inside the dbpath.
 *
 * This test does not make sense for in-memory storage engines, since they will not produce any data
 * files.
 * @tags: [requires_persistence]
 */

(function() {
'use strict';

const baseDir = "jstests_directoryperdb";
const dbpath = MongoRunner.dataPath + baseDir + "/";

const isDirectoryPerDBSupported =
    jsTest.options().storageEngine == "wiredTiger" || !jsTest.options().storageEngine;

const m = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: ''});

if (!isDirectoryPerDBSupported) {
    assert.isnull(m, 'storage engine without directoryperdb support should fail to start up');
    return;
} else {
    assert(m, 'storage engine with directoryperdb support failed to start up');
}

const getFooDir = function() {
    return listFiles(dbpath).filter(function(path) {
        return path.name.endsWith("/foo");
    });
};

const checkFooDirExists = function() {
    const files = getFooDir();
    assert.eq(
        1,
        files.length,
        "dbpath did not contain 'foo' directory when it should have: " + tojson(listFiles(dbpath)));
    assert.gt(listFiles(files[0].name).length, 0);
};

const checkFooDirRemoved = function() {
    checkLog.containsJson(db.getMongo(), 4888200, {db: "foo"});
    const files = getFooDir();
    assert.eq(0,
              files.length,
              "dbpath contained 'foo' directory when it should have been removed: " +
                  tojson(listFiles(dbpath)));
};

const db = m.getDB("foo");
assert.commandWorked(db.bar.insert({x: 1}));
checkFooDirExists();

// Test that dropping the last collection in the database causes the database directory to be
// removed.
assert(db.bar.drop());
checkFooDirRemoved();

// Test that dropping the entire database causes the database directory to be removed.
assert.commandWorked(db.bar.insert({x: 1}));
checkFooDirExists();
assert.commandWorked(db.dropDatabase());
checkFooDirRemoved();

MongoRunner.stopMongod(m);

// Subsequent attempt to start server using same dbpath without directoryperdb should fail.
assert.isnull(MongoRunner.runMongod({dbpath: dbpath, restart: true}));
}());
