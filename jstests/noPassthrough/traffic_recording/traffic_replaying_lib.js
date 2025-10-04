/*
 * Utilities for traffic replaying tests.
 */

export function createDirectories(baseDir, customSubDir) {
    const pathSep = _isWindows() ? "\\" : "/";
    const recordingDirGlobal = MongoRunner.toRealDir("$dataDir" + pathSep + baseDir);
    const recordingDir = MongoRunner.toRealDir(recordingDirGlobal + pathSep + customSubDir + pathSep);
    jsTest.log("Creating a new directory: " + recordingDirGlobal);
    jsTest.log("Creating a new directory: " + recordingDir);
    assert(mkdir(recordingDirGlobal));
    assert(mkdir(recordingDir));
    return {recordingDirGlobal, recordingDir};
}

export function cleanUpDirectory(directoryPath) {
    jsTest.log(`Cleaning up directory: ${directoryPath}`);
    removeFile(directoryPath); // Deletes the directory and its contents
}

/**
 * This function sets up a mongod instance, runs 'opsToRecord' callback function to run some
 * operations against the mongod instance with a default namespace. Operations in 'opsToRecord' are
 * recorded. The running mongod instance(including the collection) and the recording file path are
 * returned.
 */
export function recordOperations(recordingDirGlobal, customRecordingDir, opsToRecord) {
    const opts = {auth: "", setParameter: "trafficRecordingDirectory=" + recordingDirGlobal};
    const mongodInstance = MongoRunner.runMongod(opts);

    const adminDB = mongodInstance.getDB("admin");
    const testDB = mongodInstance.getDB("test");
    const coll = testDB.getCollection("coll");

    adminDB.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    adminDB.auth("admin", "pass");

    assert.commandWorked(adminDB.runCommand({startTrafficRecording: 1, destination: customRecordingDir}));

    const dbContext = {adminDB, testDB, coll, serverURI: `mongodb://${mongodInstance.host}`};

    opsToRecord(dbContext);

    const serverStatus = assert.commandWorked(testDB.runCommand({serverStatus: 1}));
    const recordingFilePath = serverStatus.trafficRecording.recordingDir;

    assert.commandWorked(adminDB.runCommand({stopTrafficRecording: 1}));

    return {mongodInstance, coll, recordingFilePath};
}
