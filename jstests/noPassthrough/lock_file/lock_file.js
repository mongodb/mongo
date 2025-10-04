// The mongod process should always create a mongod.lock file in the data directory
// containing the process ID regardless of the storage engine requested.

// Ensures that mongod.lock exists and returns size of file.
function getMongodLockFileSize(dir) {
    let files = listFiles(dir);
    for (let i in files) {
        let file = files[i];
        if (!file.isDirectory && file.baseName == "mongod.lock") {
            return file.size;
        }
    }
    assert(false, "mongod.lock not found in data directory " + dir);
}

let baseName = jsTestName();
let dbpath = MongoRunner.dataPath + baseName + "/";

// Test framework will append --storageEngine command line option.
let mongod = MongoRunner.runMongod({dbpath: dbpath});
assert.neq(0, getMongodLockFileSize(dbpath), "mongod.lock should not be empty while server is running");

MongoRunner.stopMongod(mongod);

// mongod.lock must be empty after shutting server down.
assert.eq(0, getMongodLockFileSize(dbpath), "mongod.lock not truncated after shutting server down");
