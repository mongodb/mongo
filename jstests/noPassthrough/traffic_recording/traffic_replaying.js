// tests for the traffic recording and replaying commands.
// @tags: [requires_auth]

import {
    createDirectories,
    cleanUpDirectory,
    recordOperations,
} from "jstests/noPassthrough/traffic_recording/traffic_replaying_lib.js";

function parseRecordedTraffic(recordingFilePath) {
    const recordedTraffic = convertTrafficRecordingToBSON(recordingFilePath);
    const opTypes = {};
    recordedTraffic.forEach((obj) => {
        const opType = obj.opType;
        opTypes[opType] = (opTypes[opType] || 0) + 1;
    });
    return {opTypes, recordedTraffic};
}

function recordAndParseOperations(recordingDirGlobal, customRecordingDir, workflowCallback) {
    const {mongodInstance, coll, recordingFilePath} = recordOperations(
        recordingDirGlobal,
        customRecordingDir,
        workflowCallback,
    );
    const serverURI = `mongodb://${mongodInstance.host}`;

    MongoRunner.stopMongod(mongodInstance, null, {user: "admin", pwd: "pass"});

    return {
        ...parseRecordedTraffic(recordingFilePath),
        recordingFilePath,
        serverURI,
        recordingDirGlobal,
    };
}

function runInstances(baseDir, customSubDir, workflowCallback) {
    const {recordingDirGlobal, recordingDir} = createDirectories(baseDir, customSubDir);
    return recordAndParseOperations(recordingDirGlobal, customSubDir, workflowCallback);
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

// First, test the native method with _invalid_ input, and check appropriate errors are returned.

// Too few arguments
assert.throwsWithCode(() => replayWorkloadRecordingFile(), ErrorCodes.FailedToParse);
assert.throwsWithCode(() => replayWorkloadRecordingFile("asdf"), ErrorCodes.FailedToParse);

// Too many arguments
assert.throwsWithCode(() => replayWorkloadRecordingFile("foo", "bar", "baz"), ErrorCodes.FailedToParse);

// Invalid argument types
const invalidValues = [1.1, {}, [], null, undefined, MinKey, MaxKey];
for (let dirName of invalidValues) {
    for (let clusterSpec of invalidValues) {
        assert.throwsWithCode(() => replayWorkloadRecordingFile(dirName, clusterSpec), ErrorCodes.FailedToParse, [], {
            dirName,
            clusterSpec,
        });
    }
}
assert.throwsWithCode(() => replayWorkloadRecordingFile("foo", "bar", "baz"), ErrorCodes.FailedToParse);

// Invalid directory
assert.throwsWithCode(() => replayWorkloadRecordingFile("asdf", "asdf"), ErrorCodes.FileOpenFailed);

// Empty directory
// Burn-in runs multiple instances; ensure directory is unique.
let realDirectory = "real_directory_" + UUID().hex();
mkdir(realDirectory);
// The cluster spec is _not_ valid here, but is currently not validated until
// first required to connect.
// TODO: SERVER-108026 validate the cluster string earlier.
assert.doesNotThrow(() => replayWorkloadRecordingFile(realDirectory, "asdf"));
removeFile(realDirectory);

// ======================================================================================== //
// Recording
const initialResults = runInstances("traffic_recording_" + UUID().hex(), "recordings", defaultOperationsLambda);
assert.eq(initialResults.opTypes["serverStatus"], 1);
assert.eq(initialResults.opTypes["insert"], 2);
assert.eq(initialResults.opTypes["find"], 2);
assert.eq(initialResults.opTypes["delete"], 1);
assert.eq(initialResults.opTypes["aggregate"], 1);
assert.eq(initialResults.opTypes["update"], 1);
assert.eq(initialResults.opTypes["stopTrafficRecording"], 1);
assert.eq(initialResults.opTypes["sessionStart"], 1);
assert.eq(initialResults.opTypes["sessionEnd"], 1);

// ======================================================================================== //
// Replaying
const replayResults = runInstances("replayed_recording_" + UUID().hex(), "replayed_recordings", (dbContext) => {
    const {
        testDB,
        coll,
        serverURI, // uri of the shadow cluster server.
    } = dbContext;
    const recordingFilePath = initialResults.recordingFilePath;
    const replayingFilePath = replayWorkloadLambda(recordingFilePath, serverURI);
    return replayingFilePath;
});
// in order to compute the filepath, we issue a server status inside runInstances, this plus the
// one recorded will bring total count to 2.
assert.eq(replayResults.opTypes["serverStatus"], 2);
assert.eq(replayResults.opTypes["insert"], 2);
assert.eq(replayResults.opTypes["find"], 2);
assert.eq(replayResults.opTypes["delete"], 1);
assert.eq(replayResults.opTypes["aggregate"], 1);
assert.eq(replayResults.opTypes["update"], 1);
assert.eq(replayResults.opTypes["stopTrafficRecording"], 1);
assert.eq(initialResults.opTypes["sessionStart"], 1);
assert.eq(initialResults.opTypes["sessionEnd"], 1);
// ======================================================================================== //

cleanUpDirectory(initialResults.recordingDirGlobal);
cleanUpDirectory(replayResults.recordingDirGlobal);
