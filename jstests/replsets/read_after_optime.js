// Test read after opTime functionality with maxTimeMS.

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();
    var config = replTest.getReplSetConfigFromNode();

    var runTest = function(testDB, primaryConn) {
        primaryConn.getDB('test').user.insert({x: 1}, {writeConcern: {w: 2}});

        var localDB = primaryConn.getDB('local');

        var oplogTS = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).next();
        var twoSecTS = new Timestamp(oplogTS.ts.getTime() + 2, 0);

        var term = -1;
        if (config.protocolVersion === 1) {
            term = oplogTS.t;
        }

        // Test timeout with maxTimeMS
        var runTimeoutTest = function() {
            var timeoutResult = assert.commandFailedWithCode(testDB.runCommand({
                find: 'user',
                filter: {x: 1},
                readConcern: {afterOpTime: {ts: twoSecTS, t: term}},
                maxTimeMS: 5000,
            }),
                                                             ErrorCodes.ExceededTimeLimit);
            assert.gt(timeoutResult.waitedMS, 500);
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
        var insertFunc = startParallelShell(
            "sleep(2100); db.user.insert({ y: 1 }, { writeConcern: { w: 2 }});", primaryConn.port);

        var res = assert.commandWorked(testDB.runCommand({
            find: 'user',
            filter: {x: 1},
            readConcern: {
                afterOpTime: {ts: twoSecTS, t: term},
            },
            maxTimeMS: 10 * 1000,
        }));

        assert.eq(null, res.code);
        assert.gt(res.waitedMS, 0);

        insertFunc();
    };

    var primary = replTest.getPrimary();
    runTest(primary.getDB('test'), primary);
    runTest(replTest.getSecondary().getDB('test'), primary);

    replTest.stopSet();

})();
