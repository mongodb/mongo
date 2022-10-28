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
const dbname = "foo";

const isDirectoryPerDBSupported =
    jsTest.options().storageEngine == "wiredTiger" || !jsTest.options().storageEngine;

const m = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: ''});

if (!isDirectoryPerDBSupported) {
    assert.isnull(m, 'storage engine without directoryperdb support should fail to start up');
    return;
} else {
    assert(m, 'storage engine with directoryperdb support failed to start up');
}

const getDir = function(dbName, dbDirPath) {
    return listFiles(dbDirPath).filter(function(path) {
        return path.name.endsWith(dbName);
    });
};

const checkDirExists = function(dbName, dbDirPath) {
    const files = getDir(dbName, dbDirPath);
    assert.eq(1,
              files.length,
              "dbpath did not contain '" + dbName +
                  "' directory when it should have: " + tojson(listFiles(dbDirPath)));
    assert.gt(listFiles(files[0].name).length, 0);
};

const checkDirRemoved = function(dbName, dbDirPath) {
    checkLog.containsJson(db.getMongo(), 4888200, {db: dbName});
    assert.soon(
        function() {
            const files = getDir(dbName, dbDirPath);
            if (files.length == 0) {
                return true;
            } else {
                return false;
            }
        },
        "dbpath contained '" + dbName +
            "' directory when it should have been removed:" + tojson(listFiles(dbDirPath)),
        20 * 1000);  // The periodic task to run data table cleanup runs once a second.
};

const db = m.getDB(dbname);
assert.commandWorked(db.bar.insert({x: 1}));
checkDirExists(dbname, dbpath);

// Test that dropping the last collection in the database causes the database directory to be
// removed.
assert(db.bar.drop());
checkDirRemoved(dbname, dbpath);

// Test that dropping the entire database causes the database directory to be removed.
assert.commandWorked(db.bar.insert({x: 1}));
checkDirExists(dbname, dbpath);
assert.commandWorked(db.dropDatabase());
checkDirRemoved(dbname, dbpath);

MongoRunner.stopMongod(m);

// Subsequent attempt to start server using same dbpath without directoryperdb should fail.
assert.throws(() => MongoRunner.runMongod({dbpath: dbpath, restart: true}));
}());
