// Test read after opTime functionality with maxTimeMS.

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();
    var config = replTest.getReplSetConfigFromNode();

    var runTest = function(testDB, primaryConn) {
        var dbName = testDB.getName();
        assert.writeOK(primaryConn.getDB(dbName).user.insert({x: 1}, {writeConcern: {w: 2}}));

        var localDB = primaryConn.getDB('local');

        var oplogTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).next();
        var twoKSecTS = new Timestamp(oplogTS.ts.getTime() + 2000, 0);

        var term = -1;
        if (config.protocolVersion === 1) {
            term = oplogTS.t;
        }

        // Test timeout with maxTimeMS
        var runTimeoutTest = function() {
            assert.commandFailedWithCode(testDB.runCommand({
                find: 'user',
                filter: {x: 1},
                readConcern: {afterOpTime: {ts: twoKSecTS, t: term}},
                maxTimeMS: 5000,
            }),
                                         ErrorCodes.ExceededTimeLimit);
        };

        // Run the time out test 3 times with replication debug log level increased to 2
        // for first and last run. The time out message should be logged twice.
        testDB.setLogLevel(2, 'command');
        runTimeoutTest();
        testDB.setLogLevel(0, 'command');

        var msg = 'Command on database ' + testDB.getName() +
            ' timed out waiting for read concern to be satisfied. Command:';
        checkLog.containsWithCount(testDB.getMongo(), msg, 1);

        // Read concern timed out message should not be logged.
        runTimeoutTest();

        testDB.setLogLevel(2, 'command');
        runTimeoutTest();
        testDB.setLogLevel(0, 'command');

        checkLog.containsWithCount(testDB.getMongo(), msg, 2);

        // Test read on future afterOpTime that will eventually occur.
        primaryConn.getDB(dbName).parallelShellStarted.drop();
        oplogTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).next();
        var insertFunc = startParallelShell('let testDB = db.getSiblingDB("' + dbName + '"); ' +
                                                'sleep(3000); ' +
                                                'testDB.user.insert({y: 1});',
                                            primaryConn.port);

        var twoSecTS = new Timestamp(oplogTS.ts.getTime() + 2, 0);
        var res = assert.commandWorked(testDB.runCommand({
            find: 'user',
            filter: {y: 1},
            readConcern: {
                afterOpTime: {ts: twoSecTS, t: term},
            },
            maxTimeMS: 90 * 1000,
        }));

        assert.eq(null, res.code);
        assert.eq(res.cursor.firstBatch[0].y, 1);
        insertFunc();
    };

    var primary = replTest.getPrimary();
    jsTest.log("test1");
    runTest(primary.getDB('test1'), primary);
    jsTest.log("test2");
    runTest(replTest.getSecondary().getDB('test2'), primary);

    replTest.stopSet();
})();
