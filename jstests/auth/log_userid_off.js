/**
 * Tests that logged users will not show up in the log.
 *
 * @param mongo {Mongo} connection object.
 */
var doTest = function (mongo, callSetParam) {
    var TEST_USER = 'foo';
    var TEST_PWD = 'bar';
    var testDB = mongo.getDB('test');

    testDB.addUser(TEST_USER, TEST_PWD, jsTest.basicUserRoles);
    testDB.auth(TEST_USER, TEST_PWD);

    testDB.runCommand({ dbStats: 1 });

    var log = testDB.adminCommand({ getLog: 'global' });
    log.log.forEach(function(line) {
        assert.eq(-1, line.indexOf('user: foo@'), 'user logged: ' + line);
    });

    // logUserIds should not be settable
    var res = testDB.runCommand({ setParameter: 1, logUserIds: 1 });
    assert(!res.ok);

    testDB.runCommand({ dbStats: 1 });

    log = testDB.adminCommand({ getLog: 'global' });
    log.log.forEach(function(line) {
        assert.eq(-1, line.indexOf('user: foo@'), 'user logged: ' + line);
    });
};

var mongo = MongoRunner.runMongod({ verbose: 5 });
doTest(mongo);
MongoRunner.stopMongod(mongo.port);

var st = new ShardingTest({ shards: 1, verbose: 5 });
doTest(st.s);
st.stop();

