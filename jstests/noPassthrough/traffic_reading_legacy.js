// tests for the traffic_recording commands.
(function() {
    var baseName = "jstests_traffic_recording";

    // Variables for this test
    const recordingDir = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    const recordingFile = "recording.txt";
    const recordingFilePath = MongoRunner.toRealDir(recordingDir + "/" + recordingFile);

    // Create the recording directory if it does not already exist
    mkdir(recordingDir);

    // Create the options and run mongod
    var opts = {auth: "", setParameter: "trafficRecordingDirectory=" + recordingDir};
    m = MongoRunner.runMongod(opts);

    // Get the port of the host
    var serverPort = m.port;

    // Set the readMode and writeMode to legacy
    m.forceReadMode("legacy");
    m.forceWriteMode("legacy");

    // Create necessary users
    adminDB = m.getDB("admin");
    const testDB = m.getDB("test");
    const coll = testDB.getCollection("foo");
    adminDB.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    adminDB.auth("admin", "pass");

    // Start recording traffic
    assert.commandWorked(
        adminDB.runCommand({'startRecordingTraffic': 1, 'filename': 'recording.txt'}));

    // Run a few commands
    testDB.runCommand({"serverStatus": 1});
    coll.insert({"name": "foo biz bar"});
    coll.findOne();
    coll.insert({"name": "foo bar"});
    coll.findOne({"name": "foo bar"});
    coll.deleteOne({});

    // Stop recording traffic
    assert.commandWorked(testDB.runCommand({'stopRecordingTraffic': 1}));

    // Shutdown Mongod
    MongoRunner.stopMongod(m, null, {user: 'admin', pwd: 'password'});

    // Counters
    var opCodes = {};

    // Pass filepath to traffic_reader helper method to get recorded info in BSON
    var res = convertTrafficRecordingToBSON(recordingFilePath);

    // Iterate through the results and assert the above commands are properly recorded
    res.forEach((obj) => {
        opCodes[obj["rawop"]["header"]["opcode"]] =
            (opCodes[obj["rawop"]["header"]["opcode"]] || 0) + 1;
        assert.eq(obj["seenconnectionnum"], 1);
        var responseTo = obj["rawop"]["header"]["responseto"];
        if (responseTo == 0) {
            assert.eq(obj["destendpoint"], serverPort.toString());
        } else {
            assert.eq(obj["srcendpoint"], serverPort.toString());
        }
    });

    // ensure legacy operations worked properly
    assert.eq(opCodes[2002], 2);
    assert.eq(opCodes[2006], 1);

})();
