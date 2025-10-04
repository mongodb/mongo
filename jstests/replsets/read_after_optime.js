// Test read after opTime functionality with maxTimeMS.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
let config = replTest.getReplSetConfigFromNode();

let runTest = function (testDB, primaryConn) {
    let dbName = testDB.getName();
    assert.commandWorked(primaryConn.getDB(dbName).user.insert({x: 1}, {writeConcern: {w: 2}}));

    let localDB = primaryConn.getDB("local");

    let oplogTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).next();
    let twoKSecTS = new Timestamp(oplogTS.ts.getTime() + 2000, 0);

    let term = oplogTS.t;

    // Test timeout with maxTimeMS
    let runTimeoutTest = function () {
        assert.commandFailedWithCode(
            testDB.runCommand({
                find: "user",
                filter: {x: 1},
                readConcern: {afterOpTime: {ts: twoKSecTS, t: term}},
                maxTimeMS: 5000,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );
    };

    // Run the time out test 3 times with replication debug log level increased to 2
    // for first and last run. The time out message should be logged twice.
    testDB.setLogLevel(2, "command");
    runTimeoutTest();
    testDB.setLogLevel(0, "command");

    const msg = new RegExp(
        `Command timed out waiting for read concern to be satisfied.*"attr":{"db":"${testDB.getName()}",*`,
    );

    checkLog.containsWithCount(testDB.getMongo(), msg, 1);
    // Clear the log to not fill up the ramlog
    assert.commandWorked(testDB.adminCommand({clearLog: "global"}));

    // Read concern timed out message should not be logged.
    runTimeoutTest();

    testDB.setLogLevel(2, "command");
    runTimeoutTest();
    testDB.setLogLevel(0, "command");

    checkLog.containsWithCount(testDB.getMongo(), msg, 1);

    // Test read on future afterOpTime that will eventually occur.
    primaryConn.getDB(dbName).parallelShellStarted.drop();
    oplogTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).next();
    let insertFunc = startParallelShell(
        'let testDB = db.getSiblingDB("' + dbName + '"); ' + "sleep(3000); " + "testDB.user.insert({y: 1});",
        primaryConn.port,
    );

    let twoSecTS = new Timestamp(oplogTS.ts.getTime() + 2, 0);
    let res = assert.commandWorked(
        testDB.runCommand({
            find: "user",
            filter: {y: 1},
            readConcern: {
                afterOpTime: {ts: twoSecTS, t: term},
            },
            maxTimeMS: 90 * 1000,
        }),
    );

    assert.eq(null, res.code);
    assert.eq(res.cursor.firstBatch[0].y, 1);
    insertFunc();
};

let primary = replTest.getPrimary();
jsTest.log("test1");
runTest(primary.getDB("test1"), primary);
jsTest.log("test2");
runTest(replTest.getSecondary().getDB("test2"), primary);

replTest.stopSet();
