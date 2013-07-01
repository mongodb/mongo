/**
 * This file tests that "user:<username>@<db>" shows up in the logs.
 */

// TODO(schwerin) Re-enable this test after resolving corresponding TODO in mongo/util/log.cpp.
if (0) {

/**
 * Extracts information from a mongod/mongos log entry.
 *
 * @param line {string} a single line of log.
 *
 * @return {Object} format:
 *
 *     {
 *       id: <string>, // thread id of the log line.
 *       // list of users logged in. Can be empty.
 *       users: <Object> // map of db name to user name
 *     }
 */
var parseLog = function(line) {
    var THREAD_ID_PATTERN = / [012]?\d:\d\d:\d\d\.\d\d\d \[(.+)\] /;
    var ID_USER_PATTERN = new RegExp(THREAD_ID_PATTERN.source + 'user:([^ ]*) ');
    var res = THREAD_ID_PATTERN.exec(line);

    if (res == null) {
        return null;
    }

    var logInfo = { id: res[1], users: {} };

    var userLog = null;
    res = ID_USER_PATTERN.exec(line);

    if (res != null) {
        userLog = res[2];
        // should not have trailing commas
        assert.neq(',', userLog[userLog.length - 1], 'Bad user log list format: ' + line);

        userLog.split(',').forEach(function(userData) {
            var userAndDB = userData.split('@');
            assert.eq(2, userAndDB.length, 'Bad user db pair format: ' + userData +
             ' from line: ' + line);
            logInfo.users[userAndDB[1]] = userAndDB[0];
        });
    }

    return logInfo;
};

/**
 * Performs a series of test on user id logging.
 *
 * @param conn1 {Mongo} the connection object to use for logging in users.
 * @param conn2 {Mongo} another connection object different from conn1.
 */
var doTest = function(conn1, conn2) {
    var connInfo1 = {
        id: null, // thread id of this connection
        mongo: conn1, // connection object
        users: {} // contains authenticated users represented as a map of db to user names.
    };

    var connInfo2 = {
      id: null, mongo: conn2, users: {}
    };

    var conn1Auth = [
        { user: 'foo', pwd: 'bar', db: 'test' },
        { user: 'chun', pwd: 'li', db: 'sf' }
    ];

    var conn2Auth = [
        { user: 'root', pwd: 'ugat', db: 'admin' },
        { user: 'elbow', pwd: 'freeze', db: 'bboy' }
    ];

    var loginUser = function(connInfo, connAuth) {
        var db = connInfo.mongo.getDB(connAuth.db);
        db.addUser(connAuth.user, connAuth.pwd);
        db.auth(connAuth.user, connAuth.pwd);
        connInfo.users[connAuth.db] = connAuth.user;
    };

    var logoutUser = function(connInfo, connAuth) {
        var db = connInfo.mongo.getDB(connAuth.db);
        db.runCommand({ logout: 1 });
        delete connInfo.users[connAuth.db];
    };

    /**
     * Performs a couple of test to make sure that the format of the log is correct.
     * Also checks that whether the right users show up in the logs.
     *
     * @param log {Array.<string>} list of log lines to check.
     * @param connInfo {Object}
     */
    var checkLogs = function(log, connInfo) {
        var foundOne = false;

        /**
         * @return true if the logInfo contains the same users as connIfo.
         */
        var checkUsers = function(logInfo) {
            for (var db in logInfo.users) {
                if (logInfo.users.hasOwnProperty(db) &&
                    logInfo.users[db] != connInfo.users[db]) {
                    return false;
                }
            }

            for (db in connInfo.users) {
                if (connInfo.users.hasOwnProperty(db) &&
                    logInfo.users[db] != connInfo.users[db]) {
                    return false;
                }
            }

            return true;
        };

        var hasUser = function(logInfo) {
            for (var db in logInfo.users) {
                if (logInfo.users.hasOwnProperty(db)) {
                    return true;
                }
            }

            return false;
        };

        log.forEach(function(line) {
            var logInfo = parseLog(line);

            if (logInfo == null) return;
            if (connInfo.id == null) {
                if (checkUsers(logInfo)) {
                    connInfo.id = logInfo.id;
                    foundOne = true;
                }

                return;
            }

            if (logInfo.id == connInfo.id) {
                foundOne = true;
                assert(checkUsers(logInfo), 'logged users does not match [' +
                    tojson(connInfo.users) + '], log: ' + line);
            }
            else if(hasUser(logInfo)) {

                assert(!checkUsers(logInfo), 'Unexpected user log on another thread: ' + line);
            }
        });

        assert(foundOne, 'User log not found in: ' + tojson(log));
    };

    var testDB1 = connInfo1.mongo.getDB('test');
    var testDB2 = connInfo2.mongo.getDB('test');

    // Note: The succeeding tests should not be re-ordered.
    (function() {
        jsTest.log('Test single user on 1 connection.');
        loginUser(connInfo1, conn1Auth[0]);
        testDB1.runCommand({ dbStats: 1 });
        var log = testDB1.adminCommand({ getLog: 'global' });
        checkLogs(log.log, connInfo1);
    })();

    (function() {
        jsTest.log('Test multiple conn with 1 user each');
        loginUser(connInfo2, conn2Auth[0]);
        testDB2.runCommand({ dbStats: 1 });
        var log = testDB1.adminCommand({ getLog: 'global' });
        checkLogs(log.log, connInfo2);
    })();

    (function(){
        jsTest.log('Test multiple conn with 1 multiple user');
        loginUser(connInfo1, conn1Auth[1]);
        var log = testDB1.adminCommand({ getLog: 'global' });
        var lastLogLine = log.log.pop(); // Used for trimming out logs before this point.
        testDB1.runCommand({ dbStats: 1 });
        log = testDB1.adminCommand({ getLog: 'global' });

        // Remove old log entries.
        while (log.log.shift() != lastLogLine) { }
        assert(log.log.length > 0);
        checkLogs(log.log, connInfo1);
    })();

    (function(){
        jsTest.log('Test multiple conn with multiple users each');
        loginUser(connInfo2, conn2Auth[1]);
        var log = testDB2.adminCommand({ getLog: 'global' });
        var lastLogLine = log.log.pop(); // Used for trimming out logs before this point.
        testDB1.runCommand({ dbStats: 1 });
        log = testDB2.adminCommand({ getLog: 'global' });

        // Remove old log entries.
        while (log.log.shift() != lastLogLine) { }
        assert(log.log.length > 0);
        checkLogs(log.log, connInfo2);
    })();

    (function(){
        // Case for logout older user first.
        jsTest.log('Test log line will not show foo');
        logoutUser(connInfo1, conn1Auth[0]);
        var log = testDB1.adminCommand({ getLog: 'global' });
        var lastLogLine = log.log.pop(); // Used for trimming out logs before this point.
        testDB1.runCommand({ dbStats: 1 });
        log = testDB1.adminCommand({ getLog: 'global' });

        // Remove old log entries.
        while (log.log.shift() != lastLogLine) { }
        assert(log.log.length > 0);
        checkLogs(log.log, connInfo1);
    })();

    (function(){
        jsTest.log('Test that log for conn1 will not show \'user:\'');
        logoutUser(connInfo1, conn1Auth[1]);
        var log = testDB1.adminCommand({ getLog: 'global' });
        var lastLogLine = log.log.pop(); // Used for trimming out logs before this point.
        testDB1.runCommand({ dbStats: 1 });
        log = testDB1.adminCommand({ getLog: 'global' });

        // Remove old log entries.
        while (log.log.shift() != lastLogLine) { }
        assert(log.log.length > 0);
        checkLogs(log.log, connInfo1);
    })();

    (function(){
        // Case for logout newer user first.
        jsTest.log('Test log line will not show elbow');
        logoutUser(connInfo2, conn2Auth[1]);
        var log = testDB2.adminCommand({ getLog: 'global' });
        var lastLogLine = log.log.pop(); // Used for trimming out logs before this point.
        testDB1.runCommand({ dbStats: 1 });
        log = testDB2.adminCommand({ getLog: 'global' });

        // Remove old log entries.
        while (log.log.shift() != lastLogLine) { }
        assert(log.log.length > 0);
        checkLogs(log.log, connInfo2);
    })();
};

var mongo = MongoRunner.runMongod({ verbose: 5, setParameter: 'logUserIds=1' });
doTest(mongo, new Mongo(mongo.host));
MongoRunner.stopMongod(mongo.port);

var st = new ShardingTest({ shards: 1, verbose: 5,
    other: { mongosOptions: { setParameter: 'logUserIds=1' }}});
doTest(st.s, new Mongo(st.s.host));
st.stop();

}
