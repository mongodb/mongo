// tests for the traffic recording and replaying commands.
// @tags: [requires_auth]
function createDirectories(baseDir, customSubDir) {
    const pathSep = _isWindows() ? "\\" : "/";
    const recordingDirGlobal = MongoRunner.toRealDir("$dataDir" + pathSep + baseDir);
    const recordingDir =
        MongoRunner.toRealDir(recordingDirGlobal + pathSep + customSubDir + pathSep);
    jsTest.log("Creating a new directory: " + recordingDirGlobal);
    jsTest.log("Creating a new directory: " + recordingDir);
    assert(mkdir(recordingDirGlobal));
    assert(mkdir(recordingDir));
    return {recordingDirGlobal, recordingDir};
}

function cleanUpDirectory(directoryPath) {
    jsTest.log(`Cleaning up directory: ${directoryPath}`);
    removeFile(directoryPath);  // Deletes the directory and its contents
}

function parseRecordedTraffic(recordingFilePath) {
    const recordedTraffic = convertTrafficRecordingToBSON(recordingFilePath);
    const opTypes = {};
    recordedTraffic.forEach((obj) => {
        const opType = obj.opType;
        opTypes[opType] = (opTypes[opType] || 0) + 1;
    });
    return {opTypes, recordedTraffic};
}

function recordOperations(recordingDirGlobal, customRecordingDir, workflowCallback) {
    const opts = {auth: "", setParameter: "trafficRecordingDirectory=" + recordingDirGlobal};
    const mongodInstance = MongoRunner.runMongod(opts);

    const adminDB = mongodInstance.getDB("admin");
    const testDB = mongodInstance.getDB("test");
    const coll = testDB.getCollection("foo");

    adminDB.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    adminDB.auth("admin", "pass");

    assert.commandWorked(
        adminDB.runCommand({startTrafficRecording: 1, destination: customRecordingDir}));

    const dbContext = {adminDB, testDB, coll, serverURI: `mongodb://${mongodInstance.host}`};

    workflowCallback(dbContext);

    const serverStatus = assert.commandWorked(testDB.runCommand({serverStatus: 1}));
    const recordingFilePath = serverStatus.trafficRecording.recordingDir;

    assert.commandWorked(adminDB.runCommand({stopTrafficRecording: 1}));

    MongoRunner.stopMongod(mongodInstance, null, {user: "admin", pwd: "pass"});

    return {
        ...parseRecordedTraffic(recordingFilePath),
        recordingFilePath,
        serverURI: `mongodb://${mongodInstance.host}`,
        recordingDirGlobal,
    };
}

function runInstances(baseDir, customSubDir, workflowCallback) {
    const {recordingDirGlobal, recordingDir} = createDirectories(baseDir, customSubDir);
    return recordOperations(recordingDirGlobal, customSubDir, workflowCallback);
}

const defaultOperationsLambda = (dbContext) => {
    const {testDB, coll} = dbContext;
    assert.commandWorked(coll.insert({name: "foo biz bar"}));
    assert.eq("foo biz bar", coll.findOne().name);
    assert.commandWorked(coll.insert({name: "foo bar"}));
    assert.eq("foo bar", coll.findOne({name: "foo bar"}).name);
    assert.commandWorked(coll.deleteOne({}));
    assert.eq(1, coll.aggregate().toArray().length);
    assert.commandWorked(coll.update({}, {}));
};

const replayWorkloadLambda = (recordingFilePath, serverURI) => {
    jsTest.log("Replaying : " + recordingFilePath);
    jsTest.log("Shadow Cluster URI: " + serverURI);
    replayWorkloadRecordingFile(recordingFilePath, serverURI);
};

// ======================================================================================== //
// Recording
const initialResults = runInstances("traffic_recording", "recordings", defaultOperationsLambda);
assert.eq(initialResults.opTypes['serverStatus'], 1);
assert.eq(initialResults.opTypes['insert'], 2);
assert.eq(initialResults.opTypes['find'], 2);
assert.eq(initialResults.opTypes['delete'], 1);
assert.eq(initialResults.opTypes['aggregate'], 1);
assert.eq(initialResults.opTypes['update'], 1);
assert.eq(initialResults.opTypes['stopTrafficRecording'], 1);
// ======================================================================================== //

// ======================================================================================== //
// Replaying
const replayResults = runInstances("replayed_recording", "replayed_recordings", (dbContext) => {
    const {
        testDB,
        coll,
        serverURI  // uri of the shadow cluster server.
    } = dbContext;
    const recordingFilePath = initialResults.recordingFilePath;
    const replayingFilePath = replayWorkloadLambda(recordingFilePath, serverURI);
    return replayingFilePath;
});
// in order to compute the filepath, we issue a server status inside runInstances, this plus the
// one recorded will bring total count to 2.
assert.eq(replayResults.opTypes['serverStatus'], 2);
assert.eq(replayResults.opTypes['insert'], 2);
assert.eq(replayResults.opTypes['find'], 2);
assert.eq(replayResults.opTypes['delete'], 1);
assert.eq(replayResults.opTypes['aggregate'], 1);
assert.eq(replayResults.opTypes['update'], 1);
assert.eq(replayResults.opTypes['stopTrafficRecording'], 1);
// ======================================================================================== //

cleanUpDirectory(initialResults.recordingDirGlobal);
cleanUpDirectory(replayResults.recordingDirGlobal);
