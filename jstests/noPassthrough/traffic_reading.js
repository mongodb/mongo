// tests for the traffic_recording commands.
(function() {
    // Variables for this test
    const recordingDir = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    const recordingFile = "recording.txt";
    const recordingFilePath = MongoRunner.toRealDir(recordingDir + "/" + recordingFile);
    const replayFilePath = MongoRunner.toRealDir(recordingDir + "/replay.txt");

    assert.throws(function() {
        convertTrafficRecordingToBSON("notarealfileatall");
    });

    // Create the recording directory if it does not already exist
    mkdir(recordingDir);

    // Create the options and run mongod
    var opts = {auth: "", setParameter: "trafficRecordingDirectory=" + recordingDir};
    m = MongoRunner.runMongod(opts);

    // Get the port of the host
    var serverPort = m.port;

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
    assert.commandWorked(testDB.runCommand({"serverStatus": 1}));
    assert.commandWorked(coll.insert({"name": "foo biz bar"}));
    assert.eq("foo biz bar", coll.findOne().name);
    assert.commandWorked(coll.insert({"name": "foo bar"}));
    assert.eq("foo bar", coll.findOne({"name": "foo bar"}).name);
    assert.commandWorked(coll.deleteOne({}));
    assert.eq(1, coll.aggregate().toArray().length);
    assert.commandWorked(coll.update({}, {}));

    // Stop recording traffic
    assert.commandWorked(testDB.runCommand({'stopRecordingTraffic': 1}));

    // Shutdown Mongod
    MongoRunner.stopMongod(m, null, {user: 'admin', pwd: 'password'});

    // Counters
    var numRequest = 0;
    var numResponse = 0;
    var opTypes = {};

    // Pass filepath to traffic_reader helper method to get recorded info in BSON
    var res = convertTrafficRecordingToBSON(recordingFilePath);

    // Iterate through the results and assert the above commands are properly recorded
    res.forEach((obj) => {
        assert.eq(obj["rawop"]["header"]["opcode"], 2013);
        assert.eq(obj["seenconnectionnum"], 1);
        var responseTo = obj["rawop"]["header"]["responseto"];
        if (responseTo == 0) {
            assert.eq(obj["destendpoint"], serverPort.toString());
            numRequest++;
        } else {
            assert.eq(obj["srcendpoint"], serverPort.toString());
            numResponse++;
        }
        opTypes[obj["opType"]] = (opTypes[obj["opType"]] || 0) + 1;
    });

    // Assert there is a response for every request
    assert.eq(numResponse, numRequest);

    // Assert the opTypes were correct
    assert.eq(opTypes['isMaster'], opTypes["ismaster"]);
    assert.eq(opTypes['find'], 2);
    assert.eq(opTypes['insert'], 2);
    assert.eq(opTypes['delete'], 1);
    assert.eq(opTypes['update'], 1);
    assert.eq(opTypes['aggregate'], 1);
    assert.eq(opTypes['stopRecordingTraffic'], 1);

})();
