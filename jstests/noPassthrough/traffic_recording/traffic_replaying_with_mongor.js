// Tests for the traffic recording and replaying with 'mongor'
// @tags: [requires_auth]

import {
    cleanUpDirectory,
    createDirectories,
    recordOperations,
} from "jstests/noPassthrough/traffic_recording/traffic_replaying_lib.js";

function opsToRecord(dbContext) {
    const {testDB, coll} = dbContext;
    assert.commandWorked(coll.insert({_id: 1, val: 1}));
    assert.eq(1, coll.findOne().val);
    assert.commandWorked(coll.insert({_id: 2, val: "2"}));
    assert.commandWorked(coll.deleteOne({val: 1}));
    assert.eq(1, coll.aggregate().toArray().length);
    assert.commandWorked(coll.update({_id: 2}, {val: 2}));
}

/**
 * This function runs a set of operations against a collection (with namespace 'db.test.coll').
 * These operations will be recorded and replayed later against a shadow instance. This test will
 * compare documents on both instances to test 'mongor's replaying function.
 *
 * A recording file path and documents on the collection will be returned.
 */
function runTrafficRecording(baseDir, customSubDir) {
    const {recordingDirGlobal, recordingDir} = createDirectories(baseDir, customSubDir);
    const {mongodInstance, coll, recordingFilePath} = recordOperations(recordingDirGlobal, customSubDir, opsToRecord);

    const docs = coll.find({}).toArray();

    MongoRunner.stopMongod(mongodInstance, null, {user: "admin", pwd: "pass"});

    return {recordingFilePath, docs, recordingDirGlobal};
}

const {recordingFilePath, docs, recordingDirGlobal} = runTrafficRecording("traffic_recording", "recordings");
jsTest.log("Documents on the original instance: ");
printjson(docs);

function setupShadowInstance() {
    mkdir("shadow");
    const opts = {dbpath: "shadow"};
    const shadowMongod = MongoRunner.runMongod(opts);

    const testDB = shadowMongod.getDB("test");
    const shadowColl = testDB.getCollection("coll");
    shadowColl.drop();

    const shadowURI = `mongodb://${shadowMongod.host}`;
    return {
        shadowURI,
        shadowMongod,
        shadowColl,
    };
}
const {shadowURI, shadowMongod, shadowColl} = setupShadowInstance();

// Runs 'mongor' to replay the recorded operations against the shadow instance.
runProgram("mongor", "-i", recordingFilePath, "-t", shadowURI);

const shadowDocs = shadowColl.find().toArray();
jsTest.log("Documents on the shadow instance: ");
printjson(shadowDocs);

assert.eq(docs, shadowDocs);
assert.eq(shadowDocs.length, 1);

MongoRunner.stopMongod(shadowMongod);
cleanUpDirectory(recordingDirGlobal);
