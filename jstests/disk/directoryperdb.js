var baseDir = 'jstests_disk_directoryper';
var baseName = 'directoryperdb';
var dbpath = MongoRunner.dataPath + baseDir + '/';
var storageEngine = db.serverStatus().storageEngine.name;

// The pattern which matches the names of database files
var dbFileMatcher;
if (storageEngine == 'wiredTiger') {
    // Matches wiredTiger collection-*.wt and index-*.wt files
    dbFileMatcher = /(collection|index)-.+\.wt$/;
} else {
    assert(false, 'This test must be run against wiredTiger');
}

// Set up helper functions.
assertDocumentCount = function(db, count) {
    assert.eq(count,
              db[baseName].count(),
              'Expected ' + count + ' documents in ' + db._name + '.' + baseName + '. ' +
                  'Found: ' + tojson(db[baseName].find().toArray()));
};

/**
 * Returns the current connection which gets restarted with wiredtiger.
 */
checkDBFilesInDBDirectory = function(conn, dbToCheck) {
    if (storageEngine == 'wiredTiger') {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: '', restart: true});
    }

    var dir = dbpath + dbToCheck;
    if (storageEngine == 'wiredTiger') {
        // The KV catalog escapes non alpha-numeric characters with its UTF-8 byte sequence in
        // decimal when creating the directory on disk.
        if (dbToCheck == '&') {
            dir = dbpath + '.38';
        } else if (dbToCheck == '処') {
            dir = dbpath + '.229.135.166';
        } else if (dbToCheck == Array(22).join('処')) {
            dir = dbpath + Array(22).join('.229.135.166');
        }
    }

    files = listFiles(dir);
    var fileCount = 0;
    for (f in files) {
        if (files[f].isDirectory)
            continue;
        fileCount += 1;
        assert(dbFileMatcher.test(files[f].name),
               'In directory:' + dir + ' found unexpected file: ' + files[f].name);
    }
    assert(fileCount > 0, 'Expected more than zero nondirectory files in database directory');
    return conn;
};

/**
 * Returns the restarted connection with wiredtiger.
 */
checkDBDirectoryNonexistent = function(conn, dbToCheck) {
    if (storageEngine == 'wiredTiger') {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, directoryperdb: '', restart: true});
    }

    var files = listFiles(dbpath);
    // Check that there are no files in the toplevel dbpath.
    for (f in files) {
        if (!files[f].isDirectory) {
            assert(!dbFileMatcher.test(files[f].name),
                   'Database file' + files[f].name +
                       ' exists in dbpath after deleting all non-directoryperdb databases');
        }
    }

    // Check db directories to ensure db files in them have been destroyed.
    if (storageEngine == 'wiredTiger') {
        var dir = dbpath + dbToCheck;
        // The KV catalog escapes non alpha-numeric characters with its UTF-8 byte sequence in
        // decimal when creating the directory on disk.
        if (dbToCheck == '&') {
            dir = dbpath + '.38';
        } else if (dbToCheck == '処') {
            dir = dbpath + '.229.135.166';
        } else if (dbToCheck == Array(22).join('処')) {
            dir = dbpath + Array(22).join('.229.135.166');
        }
        var files = listFiles(dir);
        assert.eq(files.length, 0, 'Files left behind in database directory');
    }
    return conn;
};

// Start the directoryperdb instance of mongod.
var m = MongoRunner.runMongod({storageEngine: storageEngine, dbpath: dbpath, directoryperdb: ''});
// Check that the 'local' db has allocated data.
m = checkDBFilesInDBDirectory(m, 'local');

// Create database with directoryperdb.
var dbBase = m.getDB(baseName);
dbBase[baseName].insert({});
assertDocumentCount(dbBase, 1);
m = checkDBFilesInDBDirectory(m, baseName);
dbBase = m.getDB(baseName);

// Drop a database created with directoryperdb.
assert.commandWorked(dbBase.runCommand({dropDatabase: 1}));
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
if (storageEngine == 'wiredTiger') {
    var dbW = m.getDB('WiredTiger');
    assert.writeError(dbW[baseName].insert({}));
}

// Create a database named 'a' repeated 63 times.
var dbNameAA = Array(64).join('a');
var dbAA = m.getDB(dbNameAA);
assert.commandWorked(dbAA[baseName].insert({}));
assertDocumentCount(dbAA, 1);
m = checkDBFilesInDBDirectory(m, dbAA);

// Create a database named '&'.
var dbAnd = m.getDB('&');
assert.commandWorked(dbAnd[baseName].insert({}));
assertDocumentCount(dbAnd, 1);
m = checkDBFilesInDBDirectory(m, dbAnd);

// Unicode directoryperdb databases do not work on Windows.
// Disabled until https://jira.mongodb.org/browse/SERVER-16725
// is resolved.
if (!_isWindows()) {
    // Create a database named '処'.
    var dbNameU = '処';
    var dbU = m.getDB(dbNameU);
    assert.commandWorked(dbU[baseName].insert({}));
    assertDocumentCount(dbU, 1);
    m = checkDBFilesInDBDirectory(m, dbU);

    // Create a database named '処' repeated 21 times.
    var dbNameUU = Array(22).join('処');
    var dbUU = m.getDB(dbNameUU);
    assert.commandWorked(dbUU[baseName].insert({}));
    assertDocumentCount(dbUU, 1);
    m = checkDBFilesInDBDirectory(m, dbUU);
}
MongoRunner.stopMongod(m);
