// Test read after opTime functionality with maxTimeMS.

(function() {
"use strict";

var replTest = new ReplSetTest({ nodes: 2 });
replTest.startSet();

var config = replTest.getReplSetConfig();
replTest.initiate(config);
config = replTest.getConfigFromPrimary();

var runTest = function(testDB, primaryConn) {
    primaryConn.getDB('test').user.insert({ x: 1 }, { writeConcern: { w: 2 }});

    var localDB = primaryConn.getDB('local');

    var oplogTS = localDB.oplog.rs.find().sort({ $natural: -1 }).limit(1).next();
    var twoSecTS = new Timestamp(oplogTS.ts.getTime() + 2, 0);

    var term = -1;
    if (config.protocolVersion === 1) {
        term = oplogTS.t;
    }

    // Test timeout with maxTimeMS
    var res = assert.commandFailed(testDB.runCommand({
        find: 'user',
        filter: { x: 1 },
        readConcern: {
            afterOpTime: { ts: twoSecTS, t: term }
        },
        maxTimeMS: 1000
    }));

    assert.eq(50, res.code, tojson(res)); // ErrorCodes::ExceededTimeLimit
    assert.gt(res.waitedMS, 500);
    assert.lt(res.waitedMS, 2500);

    // Test read on future afterOpTime that will eventually occur.
    var insertFunc = startParallelShell(
        "sleep(2100); db.user.insert({ y: 1 }, { writeConcern: { w: 2 }});",
        primaryConn.port);

    res = assert.commandWorked(testDB.runCommand({
        find: 'user',
        filter: { x: 1 },
        readConcern: {
            afterOpTime: { ts: twoSecTS, t: term },
            maxTimeMS: 10 * 1000
        }
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
