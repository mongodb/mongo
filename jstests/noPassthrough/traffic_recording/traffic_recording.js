// tests for the traffic_recording commands.
// @tags: [requires_auth]
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getDB(client) {
    let db = client.getDB("admin");
    db.auth("admin", "pass");

    return db;
}

function runTest(client, restartCommand) {
    let db = getDB(client);

    let res = db.runCommand({"startTrafficRecording": 1, "destination": "notARealPath"});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording directory not set");

    const path = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    mkdir(path);

    client = restartCommand({trafficRecordingDirectory: path});
    db = getDB(client);

    res = db.runCommand({"startTrafficRecording": 1, "destination": "notARealPath"});
    assert.eq(res.ok, true);

    // Running the command again should fail
    res = db.runCommand({"startTrafficRecording": 1, "destination": "notARealPath"});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording already active");

    // Running the serverStatus command should return the relevant information
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], true);

    // Assert that the current file size is growing
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats2 = res["trafficRecording"];
    assert.eq(trafficStats2["running"], true);
    assert(trafficStats2["currentFileSize"] >= trafficStats["currentFileSize"]);

    // Running the stopTrafficRecording command should succeed
    res = db.runCommand({"stopTrafficRecording": 1});
    assert.eq(res.ok, true);

    // Running the stopTrafficRecording command again should fail
    res = db.runCommand({"stopTrafficRecording": 1});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording not active");

    // Running the serverStatus command should return running is false
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], false);

    return client;
}

{
    let m = MongoRunner.runMongod({auth: ""});

    let db = m.getDB("admin");

    db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    db.auth("admin", "pass");

    m = runTest(m, function (setParams) {
        if (m) {
            MongoRunner.stopMongod(m, null, {user: "admin", pwd: "pass"});
        }
        try {
            m = MongoRunner.runMongod({auth: "", setParameter: setParams});
        } catch (e) {
            return null;
        }

        m.getDB("admin").createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});

        return m;
    });

    MongoRunner.stopMongod(m, null, {user: "admin", pwd: "pass"});
}

{
    let shardTest = new ShardingTest({
        config: 1,
        mongos: 1,
        shards: 0,
    });

    runTest(shardTest.s, function (setParams) {
        shardTest.restartMongos(0, {
            restart: true,
            setParameter: setParams,
        });

        return shardTest.s;
    });

    shardTest.stop();
}

function normalize(path) {
    return path.replace(/\\/g, "/").replace(/^\w:/, "");
}

// Test that the Recorder can generate a new recording file when the current recording reaches the
// max file size.
{
    const path = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    let recordingDir = path + "/recordings/";

    removeFile(recordingDir);
    mkdir(recordingDir);

    let m = MongoRunner.runMongod({auth: "", setParameter: {trafficRecordingDirectory: path, enableTestCommands: 1}});

    let db = m.getDB("admin");

    db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    db.auth("admin", "pass");

    let coll = db[jsTestName()];
    let res = db.runCommand({"startTrafficRecording": 1, "destination": "recordings", "maxFileSize": NumberLong(100)});
    assert.eq(res.ok, true, res);

    // Note how many files exist at this point in time.
    // It may not be exactly one, it may already have exceeded the max length
    // and created a new file.
    const initialNumFiles = ls(recordingDir).length;

    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], true);
    assert.eq(trafficStats["maxFileSize"], NumberLong(100));

    // Run a few commands to cause hitting the max file size and creating a new recording file.
    assert.commandWorked(coll.insert({"foo": "bar"}));
    coll.find({"foo": "bar"});
    coll.find({"a": 1});
    // The name of the recordings uses timestamp/date. Forwarding the system time to prevent opening
    // the same recording file.
    sleep(100);

    assert.commandWorked(coll.insert({"foo": "bar", "a": 1}));
    coll.find({"a": 1});
    coll.find({"a": 2});
    coll.find({"foo": "bar"});

    // Check the server status again to ensure a different recording file was generated.
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    trafficStats = res["trafficRecording"];
    assert.gt(ls(recordingDir).length, initialNumFiles, "A new recording file should be created");

    res = db.runCommand({"stopTrafficRecording": 1});
    assert.eq(res.ok, true);

    // Validate checksums were written to disk.
    const checksums = cat(`${recordingDir}/checksum.txt`);
    const fileChecksums = checksums
        .slice(0, -1) // Strip trailing newline
        .split("\n") // Split into entries per file
        .map((line) => line.split("\t")) // Split each entry into [filename, checksum]
        .map(([fname, checksum]) => [normalize(fname), checksum]); // Normalize paths to unix style
    // There should be multiple entries
    assert(fileChecksums.length > 1);
    // Entries should not all be zero (but in principle 0 is a valid checksum).
    assert(
        fileChecksums.some(([fname, checksum]) => parseInt(checksum, 16)),
        {msg: "Incorrect checksums", fileChecksums},
    );
    // Filenames should start with the recordingDir
    assert(
        fileChecksums.every(([fname, checksum]) => fname.startsWith(normalize(recordingDir))),
        {msg: "Incorrect recording filenames in checksum file", fileChecksums, recordingDir},
    );

    // Filenames should end with .bin
    assert(
        fileChecksums.every(([fname, checksum]) => fname.endsWith(".bin")),
        {
            msg: "Incorrect recording filenames in checksum file (wrong extension)",
            fileChecksums,
            recordingDir,
        },
    );

    MongoRunner.stopMongod(m, null, {user: "admin", pwd: "pass"});

    removeFile(recordingDir);
}

{
    const dirA = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    const dirB = MongoRunner.toRealDir("$dataDir/traffic_recording_different/");

    const startMongod = () => {
        const mongod = MongoRunner.runMongod({auth: ""});
        return [mongod, () => MongoRunner.stopMongod(mongod)];
    };
    const startSharded = () => {
        let sharded = new ShardingTest({
            config: 1,
            mongos: 1,
            shards: 0,
        });
        return [sharded.s, () => sharded.stop()];
    };

    const setRecordingDirAndVerify = (db, dirname) => {
        assert.commandWorked(db.adminCommand({"setParameter": 1, "trafficRecordingDirectory": dirname}));

        assert(!fileExists(dirname + "/subdir"));

        assert.commandWorked(db.runCommand({"startTrafficRecording": 1, "destination": "subdir"}));
        assert.commandWorked(db.runCommand({"stopTrafficRecording": 1}));

        assert(fileExists(dirname + "/subdir"));
    };

    for (let start of [startMongod, startSharded]) {
        // Cleanup in case a previous run failed.
        removeFile(dirA);
        removeFile(dirB);

        // Directory set by parameter must exist; subdirectory will be created.
        mkdir(dirA);
        mkdir(dirB);

        let [m, stop] = start();
        let db = m.getDB("admin");
        db.createUser({user: "admin", pwd: "pwd", roles: ["root"], mechanisms: ["SCRAM-SHA-1"]});
        db.auth("admin", "pwd");

        setRecordingDirAndVerify(db, dirA);
        setRecordingDirAndVerify(db, dirB);

        stop();

        // Cleanup files.
        removeFile(dirA);
        removeFile(dirB);
    }
}

// Ensure startTrafficRecording validates startTime and endTime
{
    const path = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    let recordingDir = path + "/recordings/";

    removeFile(recordingDir);
    mkdir(recordingDir);

    let m = MongoRunner.runMongod({auth: "", setParameter: {trafficRecordingDirectory: path, enableTestCommands: 1}});

    let db = m.getDB("admin");

    db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    db.auth("admin", "pass");

    const nowPlusDays = (days) => {
        const now = new Date();
        const milliseconds = days * 24 * 60 * 60 * 1000;
        return new Date(now.getTime() + milliseconds);
    };

    // Validate start response when providing recording ID.
    {
        let res = assert.commandWorked(
            db.runCommand({"startTrafficRecording": 1, "destination": "recordings", "recordingID": "foobar"}),
        );
        assert(res.created);
        assert.eq(res.status, "running");
    }

    {
        // Start with the same ID is allowed, but doesn't create a new recording.
        let res = assert.commandWorked(
            db.runCommand({"startTrafficRecording": 1, "destination": "recordings", "recordingID": "foobar"}),
        );
        assert(!res.created);
        assert.eq(res.status, "running");
    }

    assert.commandWorked(db.runCommand({"stopTrafficRecording": 1}));

    // Validate start response when providing recording ID and scheduling for the future.
    {
        let res = assert.commandWorked(
            db.runCommand({
                "startTrafficRecording": 1,
                "destination": "recordings",
                "recordingID": "foobar",
                startTime: nowPlusDays(0.5),
                endTime: nowPlusDays(2),
            }),
        );
        assert(res.created);
        assert.eq(res.status, "scheduled");
    }

    {
        // Start with the same ID is allowed, but doesn't create a new recording.
        let res = assert.commandWorked(
            db.runCommand({"startTrafficRecording": 1, "destination": "recordings", "recordingID": "foobar"}),
        );
        assert(!res.created);
        assert.eq(res.status, "scheduled");
    }

    assert.commandWorked(db.runCommand({"stopTrafficRecording": 1}));

    // No start or end time - allowed.
    assert.commandWorked(db.runCommand({"startTrafficRecording": 1, "destination": "recordings"}));
    assert.commandWorked(db.runCommand({"stopTrafficRecording": 1}));

    // start time < 1 day in the future, end time > start time && end time < 10 days in the future -
    // allowed.
    assert.commandWorked(
        db.runCommand({
            "startTrafficRecording": 1,
            "destination": "recordings",
            startTime: nowPlusDays(0.5),
            endTime: nowPlusDays(2),
        }),
    );
    // Cancelling early is permitted.
    assert.commandWorked(db.runCommand({"stopTrafficRecording": 1}));

    const expectFailure = (params) => {
        assert.commandFailedWithCode(
            db.runCommand({"startTrafficRecording": 1, "destination": "recordings", ...params}),
            ErrorCodes.BadValue,
        );
    };

    // Only start.
    expectFailure({startTime: nowPlusDays(0.5)});
    // Only end.
    expectFailure({endTime: nowPlusDays(2)});
    // Starts too late.
    expectFailure({startTime: nowPlusDays(1.5), endTime: nowPlusDays(2)});
    // Ends too late.
    expectFailure({startTime: nowPlusDays(0.5), endTime: nowPlusDays(20)});
    // Both.
    expectFailure({startTime: nowPlusDays(1.5), endTime: nowPlusDays(20)});

    MongoRunner.stopMongod(m, null, {user: "admin", pwd: "pass"});

    removeFile(recordingDir);
}
