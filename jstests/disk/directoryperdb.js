let baseDir = "jstests_disk_directoryper";
let baseName = "directoryperdb";
let dbpath = MongoRunner.dataPath + baseDir + "/";
let storageEngine = "wiredTiger";

// Matches wiredTiger collection-*.wt and index-*.wt files
let dbFileMatcher = /(collection|index)-.+\.wt$/;

// Set up helper functions.
let assertDocumentCount = function (db, count) {
    assert.eq(
        count,
        db[baseName].count(),
        "Expected " +
            count +
            " documents in " +
            db._name +
            "." +
            baseName +
            ". " +
            "Found: " +
            tojson(db[baseName].find().toArray()),
    );
};

/**
 * Wait for the sub-directory for database 'dbName' in the MongoDB file directory 'dbDirPath' to be
 * deleted. MongoDB does not always delete data immediately with a catalog change.
 */
const waitForDatabaseDirectoryRemoval = function (dbName, dbDirPath) {
    // The periodic task to drop data tables for two-phase commit runs once per second and
    // in standalone mode, without timestamps, can execute table drops immediately. It should
    // only take a couple seconds for the periodic task to start removing any data tables.
    // However, slow disk access may delay the actual removal of the directory.
    // Therefore, we should be conservative and use the default timeout in assert.soon().
    assert.soon(
        function () {
            const files = listFiles(dbDirPath).filter(function (path) {
                return path.name.endsWith(dbName);
            });
            if (files.length == 0) {
                return true;
            } else {
                return false;
            }
        },
        "dbpath contained '" + dbName + "' directory when it should have been removed: " + tojson(listFiles(dbDirPath)),
    );
};

/**
 * Returns the current connection which gets restarted with wiredtiger.
 */
let checkDBFilesInDBDirectory = function (conn, dbToCheck) {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: "", restart: true});

    let dir = dbpath + dbToCheck;

    // The KV catalog escapes non alpha-numeric characters with its UTF-8 byte sequence in
    // decimal when creating the directory on disk.
    if (dbToCheck == "&") {
        dir = dbpath + ".38";
    } else if (dbToCheck == "処") {
        dir = dbpath + ".229.135.166";
    } else if (dbToCheck == Array(22).join("処")) {
        dir = dbpath + Array(22).join(".229.135.166");
    }

    let files = listFiles(dir);
    let fileCount = 0;
    for (let f in files) {
        if (files[f].isDirectory) continue;
        fileCount += 1;
        assert(dbFileMatcher.test(files[f].name), "In directory:" + dir + " found unexpected file: " + files[f].name);
    }
    assert(fileCount > 0, "Expected more than zero nondirectory files in database directory");
    return conn;
};

/**
 * Returns the restarted connection with wiredtiger.
 */
let checkDBDirectoryNonexistent = function (conn, dbToCheck) {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: "", restart: true});

    let files = listFiles(dbpath);
    // Check that there are no files in the toplevel dbpath.
    for (let f in files) {
        if (!files[f].isDirectory) {
            assert(
                !dbFileMatcher.test(files[f].name),
                "Database file" + files[f].name + " exists in dbpath after deleting all non-directoryperdb databases",
            );
        }
    }

    // Check db directories to ensure db files in them have been destroyed.
    let dir = dbpath + dbToCheck;
    // The KV catalog escapes non alpha-numeric characters with its UTF-8 byte sequence in
    // decimal when creating the directory on disk.
    if (dbToCheck == "&") {
        dir = dbpath + ".38";
    } else if (dbToCheck == "処") {
        dir = dbpath + ".229.135.166";
    } else if (dbToCheck == Array(22).join("処")) {
        dir = dbpath + Array(22).join(".229.135.166");
    }
    assert(
        !files.some((file) => file.name === dir),
        "Database " + dbToCheck + " directory " + dir + " left behind when it should have been removed",
    );
    return conn;
};

// Start the directoryperdb instance of mongod.
let m = MongoRunner.runMongod({storageEngine: storageEngine, dbpath: dbpath, directoryperdb: ""});
// Check that the 'local' db has allocated data.
m = checkDBFilesInDBDirectory(m, "local");

// Create database with directoryperdb.
let dbBase = m.getDB(baseName);
dbBase[baseName].insert({});
assertDocumentCount(dbBase, 1);
m = checkDBFilesInDBDirectory(m, baseName);
dbBase = m.getDB(baseName);

// Drop a database created with directoryperdb.
assert.commandWorked(dbBase.runCommand({dropDatabase: 1}));
waitForDatabaseDirectoryRemoval(baseName, dbpath);
assertDocumentCount(dbBase, 0);
m = checkDBDirectoryNonexistent(m, baseName);
dbBase = m.getDB(baseName);

// It should be impossible to create a database named 'journal' with directoryperdb, as that
// directory exists. This test has been disabled until SERVER-2460 is resolved.
/*
db = m.getDB( 'journal' );
assert.writeError(db[ 'journal' ].insert( {} ));
*/

// Using WiredTiger, it should be impossible to create a database named 'WiredTiger' with
// directoryperdb, as that file is created by the WiredTiger storageEngine.
let dbW = m.getDB("WiredTiger");
assert.writeError(dbW[baseName].insert({}));

// Create a database named 'a' repeated 63 times.
let dbNameAA = Array(64).join("a");
let dbAA = m.getDB(dbNameAA);
assert.commandWorked(dbAA[baseName].insert({}));
assertDocumentCount(dbAA, 1);
m = checkDBFilesInDBDirectory(m, dbAA);

// Create a database named '&'.
let dbAnd = m.getDB("&");
assert.commandWorked(dbAnd[baseName].insert({}));
assertDocumentCount(dbAnd, 1);
m = checkDBFilesInDBDirectory(m, dbAnd);

// Unicode directoryperdb databases do not work on Windows.
// Disabled until https://jira.mongodb.org/browse/SERVER-16725
// is resolved.
if (!_isWindows()) {
    // Create a database named '処'.
    let dbNameU = "処";
    let dbU = m.getDB(dbNameU);
    assert.commandWorked(dbU[baseName].insert({}));
    assertDocumentCount(dbU, 1);
    m = checkDBFilesInDBDirectory(m, dbU);

    // Create a database named '処' repeated 21 times.
    let dbNameUU = Array(22).join("処");
    let dbUU = m.getDB(dbNameUU);
    assert.commandWorked(dbUU[baseName].insert({}));
    assertDocumentCount(dbUU, 1);
    m = checkDBFilesInDBDirectory(m, dbUU);
}
MongoRunner.stopMongod(m);
