/**
 * Tests that --repair on WiredTiger correctly and gracefully handles corrupt metadata files.
 * This test should not run on debug builds because WiredTiger's diagnostic mode is enabled.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "wt_repair_corrupt_metadata";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

/**
 * This test runs repair using a version of the WiredTiger.turtle file that has checkpoint
 * information before the collection was created. The turtle file contains checkpoint
 * information about the WiredTiger.wt file, so if these two files become out of sync,
 * WiredTiger will have to attempt a salvage operation on the .wt file and rebuild the .turtle
 * file.
 *
 * The expectation is that the metadata salvage will be successful, and that the collection will
 * be recreated with all of its data.
 */
let runTest = function(mongodOptions) {
    resetDbpath(dbpath);
    jsTestLog("Running test with args: " + tojson(mongodOptions));

    const turtleFile = dbpath + "WiredTiger.turtle";
    const turtleFileWithoutCollection = dbpath + "WiredTiger.turtle.1";

    let mongod = startMongodOnExistingPath(dbpath, mongodOptions);

    // Force a checkpoint and make a copy of the turtle file.
    assert.commandWorked(mongod.getDB(baseName).adminCommand({fsync: 1}));
    jsTestLog("Making copy of metadata file before creating the collection: " +
              turtleFileWithoutCollection);
    copyFile(turtleFile, turtleFileWithoutCollection);

    let testColl = mongod.getDB(baseName)[collName];
    assert.commandWorked(testColl.insert({a: 1}));

    // Force another checkpoint before a clean shutdown.
    assert.commandWorked(mongod.getDB(baseName).adminCommand({fsync: 1}));
    MongoRunner.stopMongod(mongod);

    // Guarantee the turtle files changed between checkpoints.
    assert.neq(md5sumFile(turtleFileWithoutCollection), md5sumFile(turtleFile));

    jsTestLog("Replacing metadata file with a version before the collection existed.");
    removeFile(turtleFile);
    copyFile(turtleFileWithoutCollection, turtleFile);

    // This test characterizes the current WiredTiger salvage behaviour, which may be subject to
    // change in the future. See SERVER-41667.
    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);

    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];

    // The collection exists despite using an older turtle file because salvage is able to find
    // the table in the WiredTiger.wt file.
    assert(testColl.exists());
    // We can assert that the data exists because the salvage only took place on the metadata,
    // not the data.
    assert.eq(testColl.find({}).itcount(), 1);
    MongoRunner.stopMongod(mongod);

    // Corrupt the .turtle file in a very specific way such that the log sequence numbers are
    // invalid.
    if (_isAddressSanitizerActive()) {
        jsTestLog("Skipping log file corruption because the address sanitizer is active.");
        return;
    }

    jsTestLog("Corrupting log file metadata");

    let data = cat(turtleFile, true /* useBinaryMode */);
    let re = /checkpoint_lsn=\(([0-9,]+)\)/g;
    let newData = data.replace(re, "checkpoint_lsn=(1,2)");

    print('writing data to new turtle file: \n' + newData);
    removeFile(turtleFile);
    writeFile(turtleFile, newData, true /* useBinaryMode */);

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);

    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];

    // The collection exists despite using a salvaged turtle file because salvage is able to
    // find the table in the WiredTiger.wt file.
    assert(testColl.exists());

    // We can assert that the data exists because the salvage only took place on the
    // metadata, not the data.
    assert.eq(testColl.find({}).itcount(), 1);
    MongoRunner.stopMongod(mongod);
};

runTest({});
})();
