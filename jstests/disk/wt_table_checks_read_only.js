/**
 * Tests that the table logging settings are not changed during read only mode.
 *
 * @tags: [requires_wiredtiger]
 */
(function() {

load('jstests/disk/libs/wt_file_helper.js');

// Create a bunch of collections under various database names.
let conn = MongoRunner.runMongod({});
const dbpath = conn.dbpath;

for (let i = 0; i < 10; i++) {
    assert.commandWorked(conn.getDB(i.toString()).createCollection(i.toString()));
}

MongoRunner.stopMongod(conn);

// Option for read only mode.
let options = {queryableBackupMode: ""};

// Verifies that setTableLogging() does not get called in read only mode, otherwise the invariant
// would fire.
conn = startMongodOnExistingPath(dbpath, options);
assert(conn);
MongoRunner.stopMongod(conn);

// Create the '_wt_table_checks' file in the dbpath and ensure it doesn't get removed while in read
// only mode.
let files = listFiles(dbpath);
for (f in files) {
    assert(!files[f].name.includes("_wt_table_checks"));
}

writeFile(dbpath + "/_wt_table_checks", "");

conn = startMongodOnExistingPath(dbpath, options);
assert(conn);
MongoRunner.stopMongod(conn);

let hasWTTableChecksFile = false;
files = listFiles(dbpath);
for (f in files) {
    if (files[f].name.includes("_wt_table_checks")) {
        hasWTTableChecksFile = true;
    }
}

assert(hasWTTableChecksFile);
}());
