/**
 * Tests that MongoDB sets the WiredTiger table logging settings correctly under different
 * circumstances.
 *
 * @tags: [requires_wiredtiger]
 */
(function() {

load('jstests/disk/libs/wt_file_helper.js');

function checkTableLogSettings(conn, enabled) {
    conn.getDBNames().forEach(function(d) {
        let collNames =
            conn.getDB(d)
                .runCommand({listCollections: 1, nameOnly: true, filter: {type: "collection"}})
                .cursor.firstBatch;

        collNames.forEach(function(c) {
            let stats = conn.getDB(d).runCommand({collStats: c.name});

            let logStr = "log=(enabled=" + (enabled ? "true" : "false") + ")";
            if (d == "local") {
                if (c.name == "replset.minvalid" && !enabled) {
                    // This collection is never logged in a replica set.
                    logStr = "log=(enabled=false)";
                } else {
                    // All other collections and indexes in the 'local' database have table
                    // logging enabled always.
                    logStr = "log=(enabled=true)";
                }
            }

            assert.eq(true, stats.wiredTiger.creationString.includes(logStr));
            Object.keys(stats.indexDetails).forEach(function(i) {
                assert.eq(true, stats.indexDetails[i].creationString.includes(logStr));
            });
        });
    });
}

function checkTableChecksFileRemoved(dbpath) {
    let files = listFiles(dbpath);
    for (file of files) {
        assert.eq(false, file.name.includes("_wt_table_checks"));
    }
}

// Create a bunch of collections under various database names.
let conn = MongoRunner.runMongod({});
const dbpath = conn.dbpath;

for (let i = 0; i < 10; i++) {
    assert.commandWorked(conn.getDB(i.toString()).createCollection(i.toString()));
}

checkTableLogSettings(conn, /*enabled=*/true);
MongoRunner.stopMongod(conn);

/**
 * Test 1. Change into a single node replica set, which requires all of the table logging settings
 * to be updated. Write the '_wt_table_checks' file and check that it gets removed.
 */
jsTest.log("Test 1.");

writeFile(dbpath + "/_wt_table_checks", "");
conn = startMongodOnExistingPath(
    dbpath, {replSet: "mySet", setParameter: {logComponentVerbosity: tojson({verbosity: 1})}});
checkTableChecksFileRemoved(dbpath);

// Changing table logging settings.
checkLog.containsJson(conn, 22432);
MongoRunner.stopMongod(conn);

/**
 * Test 2. Restart in standalone mode with wiredTigerSkipTableLoggingChecksOnStartup. No table log
 * settings are updated.  Write the '_wt_table_checks' file and check that it gets removed.
 */
jsTest.log("Test 2.");
writeFile(dbpath + "/_wt_table_checks", "");
conn = startMongodOnExistingPath(dbpath, {
    setParameter: {
        wiredTigerSkipTableLoggingChecksOnStartup: true,
        logComponentVerbosity: tojson({verbosity: 1})
    }
});
checkTableChecksFileRemoved(dbpath);

// Skipping table logging checks.
checkLog.containsJson(conn, 5548302);

// Changing table logging settings.
assert(checkLog.checkContainsWithCountJson(conn, 22432, undefined, 0));
checkTableLogSettings(conn, /*enabled=*/false);
MongoRunner.stopMongod(conn);

/**
 * Test 3. Change into a single node replica set again. Table log settings are checked but none are
 * changed.  Write the '_wt_table_checks' file and check that it gets removed.
 */
jsTestLog("Test 3.");
writeFile(dbpath + "/_wt_table_checks", "");
conn = startMongodOnExistingPath(
    dbpath, {replSet: "mySet", setParameter: {logComponentVerbosity: tojson({verbosity: 1})}});
checkTableChecksFileRemoved(dbpath);

// Changing table logging settings.
assert(checkLog.checkContainsWithCountJson(conn, 22432, undefined, 0));
MongoRunner.stopMongod(conn);

/**
 * Test 4. Back to standalone. Check that the table log settings are enabled. Write the
 * '_wt_table_checks' file and check that it gets removed.
 */
jsTest.log("Test 4.");
writeFile(dbpath + "/_wt_table_checks", "");
conn = startMongodOnExistingPath(dbpath,
                                 {setParameter: {logComponentVerbosity: tojson({verbosity: 1})}});
checkTableChecksFileRemoved(dbpath);

// Changing table logging settings.
checkLog.containsJson(conn, 22432);

// Skipping table logging checks.
assert(checkLog.checkContainsWithCountJson(conn, 5548302, undefined, 0));
checkTableLogSettings(conn, /*enabled=*/true);
MongoRunner.stopMongod(conn);
}());
