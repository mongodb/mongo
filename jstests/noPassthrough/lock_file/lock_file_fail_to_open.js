// Tests that MongoD fails to start with the correct error message if mongod.lock exists in the
// dbpath.
let baseName = jsTestName();

let dbPath = MongoRunner.dataPath + baseName + "/";

// Start a MongoD just to get a lockfile in place.
let mongo1 = MongoRunner.runMongod({dbpath: dbPath, waitForConnect: true});

clearRawMongoProgramOutput();
// Start another one which should fail to start as there is already a lockfile in its
// dbpath.
assert.throws(() => MongoRunner.runMongod({dbpath: dbPath, noCleanData: true}));

let logContents = rawMongoProgramOutput(".*");
assert(
    logContents.indexOf("Unable to lock the lock file") > 0 ||
        // Windows error message is different.
        logContents.indexOf("Unable to create/open the lock file") > 0,
);

MongoRunner.stopMongod(mongo1);
