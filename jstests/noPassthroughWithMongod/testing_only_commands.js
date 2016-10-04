/**
 * Test to make sure that commands that should only work when testing commands are enabled
 * via the --enableTestCommands flag fail when that flag isn't provided.
 */

var testOnlyCommands = [
    'configureFailPoint',
    '_hashBSONElement',
    'replSetTest',
    'journalLatencyTest',
    'godinsert',
    'sleep',
    'captrunc',
    'emptycapped'
];

var assertCmdNotFound = function(db, cmdName) {
    var res = db.runCommand(cmdName);
    assert.eq(0, res.ok);
    assert.eq(59, res.code, 'expected CommandNotFound(59) error code for test command ' + cmdName);
};

var assertCmdFound = function(db, cmdName) {
    var res = db.runCommand(cmdName);
    if (!res.ok) {
        assert.neq(59,
                   res.code,
                   'test command ' + cmdName + ' should either have succeeded or ' +
                       'failed with an error code other than CommandNotFound(59)');
    }
};

jsTest.setOption('enableTestCommands', false);

var conn = MongoRunner.runMongod({});
for (i in testOnlyCommands) {
    assertCmdNotFound(conn.getDB('test'), testOnlyCommands[i]);
}
MongoRunner.stopMongod(conn.port);

// Now enable the commands
jsTest.setOption('enableTestCommands', true);

var conn = MongoRunner.runMongod({});
for (i in testOnlyCommands) {
    assertCmdFound(conn.getDB('test'), testOnlyCommands[i]);
}
MongoRunner.stopMongod(conn.port);
