// tests for the traffic_recording commands.
// @tags: [requires_auth]

// Variables for this test
const pathsep = _isWindows() ? "\\" : "/";
const recordingDirGlobal = MongoRunner.toRealDir("$dataDir" + pathsep + "traffic_recording");
const recordingDirCustom = "recordings";
const recordingDir = MongoRunner.toRealDir(recordingDirGlobal + pathsep + recordingDirCustom + pathsep);

assert.throws(function () {
    convertTrafficRecordingToBSON("notarealfileatall");
});

jsTest.log("Creating a new directory: " + recordingDirGlobal);
jsTest.log("Creating a new directory: " + recordingDir);
// Create the recording directory if it does not already exist
assert(mkdir(recordingDirGlobal));
assert(mkdir(recordingDir));

// Create the options and run mongod
let opts = {auth: "", setParameter: "trafficRecordingDirectory=" + recordingDirGlobal};
let m = MongoRunner.runMongod(opts);

// Get the port of the host
let serverPort = m.port;

// Create necessary users
let adminDB = m.getDB("admin");
const testDB = m.getDB("test");
const coll = testDB.getCollection("foo");
adminDB.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
adminDB.auth("admin", "pass");

// Start recording traffic
assert.commandWorked(adminDB.runCommand({"startTrafficRecording": 1, "destination": recordingDirCustom}));

// Run a few commands
assert.commandWorked(testDB.runCommand({"serverStatus": 1}));
assert.commandWorked(coll.insert({"name": "foo biz bar"}));
assert.eq("foo biz bar", coll.findOne().name);
assert.commandWorked(coll.insert({"name": "foo bar"}));
assert.eq("foo bar", coll.findOne({"name": "foo bar"}).name);
assert.commandWorked(coll.deleteOne({}));
assert.eq(1, coll.aggregate().toArray().length);
assert.commandWorked(coll.update({}, {}));

let serverStatus = testDB.runCommand({"serverStatus": 1});
assert("trafficRecording" in serverStatus, serverStatus);
let recordingFilePath = serverStatus["trafficRecording"]["recordingDir"];

// Stop recording traffic
assert.commandWorked(testDB.runCommand({"stopTrafficRecording": 1}));

// Shutdown Mongod
MongoRunner.stopMongod(m, null, {user: "admin", pwd: "password"});

// Counters
let numRequest = 0;
let numResponse = 0;
let opTypes = {};

jsTest.log("Recording file path: " + recordingFilePath);
// Pass filepath to traffic_reader helper method to get recorded info in BSON
let res = convertTrafficRecordingToBSON(recordingFilePath);

const eventType = {
    Regular: 0,
    SessionStart: 1,
    SessionEnd: 2,
};
let startEventRecorded = false;
let endEventRecorded = false;
// Iterate through the results and assert the above commands are properly recorded
res.forEach((obj) => {
    // The "event" field indicates the type of the event.
    if (obj["event"] === eventType.SessionStart) {
        assert(!endEventRecorded);
        startEventRecorded = true;
        assert.eq(obj["opType"], "sessionStart");
    } else if (obj["event"] === eventType.SessionEnd) {
        assert(startEventRecorded);
        endEventRecorded = true;
        assert.eq(obj["opType"], "sessionEnd");
    } else {
        // The session start event should be recorded first.
        assert.eq(startEventRecorded, true);
        // The session end event should be recorded last.
        assert.eq(endEventRecorded, false);
        assert.eq(obj["event"], eventType.Regular);
        assert.eq(obj["rawop"]["header"]["opcode"], 2013);
        assert.eq(obj["seenconnectionnum"], 1);
        opTypes[obj["opType"]] = (opTypes[obj["opType"]] || 0) + 1;
    }
});
assert(startEventRecorded);
assert(endEventRecorded);

// Assert there is a response for every request
assert.eq(numResponse, numRequest);

// Assert the opTypes were correct
assert.eq(opTypes["isMaster"], opTypes["ismaster"]);
assert.eq(opTypes["find"], 2);
assert.eq(opTypes["insert"], 2);
assert.eq(opTypes["delete"], 1);
assert.eq(opTypes["update"], 1);
assert.eq(opTypes["aggregate"], 1);
assert.eq(opTypes["stopTrafficRecording"], 1);
