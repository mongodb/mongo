/**
 * Test to make sure that commands that should only work when testing commands are enabled
 * via the --enableTestCommands flag fail when that flag isn't provided.
 */

var testOnlyCommands = ['_testDistLockWithSyncCluster',
                        '_testDistLockWithSkew',
                        '_skewClockCommand',
                        'configureFailPoint',
                        '_hashBSONElement',
                        'replSetTest',
                        'journalLatencyTest',
                        'godinsert',
                        'sleep',
                        'captrunc',
                        'emptycapped']

var assertCmdNotFound = function(db, cmdName) {
    var res = db.runCommand(cmdName);
    assert.eq(0, res.ok);
    assert(res.errmsg == 'no such cmd: ' + cmdName);
}

var assertCmdFound = function(db, cmdName) {
    var res = db.runCommand(cmdName);
    assert(res.ok || res.errmsg != 'no such cmd' + cmdName);
}

jsTest.setOption('enableTestCommands', false);

var conn = startMongodTest();
for (i in testOnlyCommands) {
    assertCmdNotFound(conn.getDB('test'), testOnlyCommands[i]);
}
MongoRunner.stopMongod(conn.port);

// Now enable the commands
jsTest.setOption('enableTestCommands', true);

var conn = startMongodTest();
for (i in testOnlyCommands) {
    assertCmdFound(conn.getDB('test'), testOnlyCommands[i]);
}
MongoRunner.stopMongod(conn.port);
