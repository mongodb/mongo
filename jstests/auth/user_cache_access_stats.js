/**
 * Test that user cache acquisition stats are appropriately
 * updated whenever a UserHandle is acquired via the user cache.
 * @tags: [
 *   requires_replication,
 *   requires_sharding
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const slowLogID = 51803;

function hasCommandLogEntry(conn, command, attributes, count) {
    let expectedLog = {command: command};
    if (Object.keys(attributes).length > 0) {
        expectedLog = Object.assign({}, expectedLog, attributes);
    }
    checkLog.containsRelaxedJson(conn, slowLogID, expectedLog, count);
}

function hasNoCommandLogEntry(conn, command, attributes) {
    let expectedLog = {command: command};
    if (Object.keys(attributes).length > 0) {
        expectedLog = Object.assign({}, expectedLog, attributes);
    }
    checkLog.containsRelaxedJson(conn, slowLogID, expectedLog, 0);
}

/**
 * Check userCacheWaitTimeMicros, durationMillis and workingMillis of the matched command log.
 * @param {*} conn Connection object to a server.
 * @param {*} command Command object for filtering the logs with "command" field.
 * @param {*} userCacheAttrs User cache attributes for filtering the logs.
 */
function checkSlowLogTimeFieldsWithUserCacheAttrs(conn, command, userCacheAttrs) {
    let expectedLog = {command: command};
    if (Object.keys(userCacheAttrs).length > 0) {
        expectedLog = Object.assign({}, expectedLog, userCacheAttrs);
    }

    let messagesOrig = checkLog.getFilteredLogMessages(conn, slowLogID, expectedLog, null, true);
    assert.eq(messagesOrig.length,
              1,
              "We should get one matched slow log with filter :" + tojson(expectedLog));

    let message = messagesOrig[0];
    assert(message.attr.authorization.hasOwnProperty("userCacheWaitTimeMicros"));
    let userCacheWaitTimeMillis =
        Math.floor(message.attr.authorization.userCacheWaitTimeMicros / 1000);
    let durationMillis = message.attr.durationMillis;
    let workingMillis = message.attr.workingMillis;

    assert.gte(durationMillis,
               userCacheWaitTimeMillis,
               "durationMillis should cover the wait time of accessing user cache.");
    assert.gte(durationMillis,
               userCacheWaitTimeMillis + workingMillis,
               "durationMillis should also cover workingMillis");
}

function runTest(conn, mode) {
    // Set the authUserCacheSleep failpoint. This causes the server to sleep for 1 second
    // every time it accesses the user cache, which provides a lower bound when checking the stats'
    // accuracy.
    const adminDB = conn.getDB('admin');
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: 'authUserCacheSleep',
        mode: 'alwaysOn',
    }));

    // Create an admin user and authenticate as the admin user.
    assert.commandWorked(adminDB.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDB.auth('admin', 'pwd'));

    // Check that authenticating as admin results in the expected log lines with the user cache
    // acquisition stats.
    const waitTimeRegex = new RegExp('^[1-9][0-9]{6,}$', 'i');
    // let expectedIsMasterCommandLog = {isMaster: 1.0};
    let expectedCommandWithUserCacheAttrs = {
        authorization: {
            startedUserCacheAcquisitionAttempts: 1,
            completedUserCacheAcquisitionAttempts: 1,
            userCacheWaitTimeMicros: waitTimeRegex,
        },
    };

    // Set logging level to 1 so that all operations are logged upon completion.
    assert.commandWorked(adminDB.runCommand({setParameter: 1, logLevel: 1}));

    // Create another database to write to and a new user with the "readWrite" and "userAdmin" roles
    // on that database.
    const testDB = conn.getDB('test');
    // Set profiling level to 2 so that profiling is enabled for the standalone test.
    if (mode === 'Standalone') {
        testDB.setProfilingLevel(0);
        testDB.system.profile.drop();
        testDB.setProfilingLevel(2);
    }

    assert.commandWorked(
        testDB.runCommand({createUser: 'testUser', pwd: 'pwd', roles: ['readWrite', 'userAdmin']}));

    // Launch a parallel shell to perform an insert operation while authenticated as 'testUser'.
    let awaitOps = startParallelShell(function() {
        const testDB = db.getSiblingDB('test');
        assert(testDB.auth('testUser', 'pwd'));
        // Insert a document into testCollection and then run a find command on it. These should
        // both succeed due to testUser's readWrite role and should not require user cache accesses.
        assert.writeOK(testDB.coll.insert({x: 1}, {writeConcern: {w: 'majority'}}));
        assert.commandWorked(testDB.runCommand({find: 'coll'}));

        // Replace testUser's 'readWrite' role with a 'read' role and try a find operation.
        assert.commandWorked(testDB.runCommand({
            revokeRolesFromUser: 'testUser',
            roles: ['readWrite'],
            writeConcern: {w: 'majority'}
        }));
        assert.commandWorked(testDB.runCommand(
            {grantRolesToUser: 'testUser', roles: ['read'], writeConcern: {w: 'majority'}}));
        assert.commandWorked(testDB.runCommand({find: 'coll'}));
    }, conn.port);

    awaitOps();

    // Check that there's a log for the successful insert command that does NOT contain
    // authorization stats (since it didn't access the user cache).
    let expectedInsertCommandLog = {insert: "coll"};
    let unexpectedCommandWithoutUserCacheAttrs = {
        authorization: {
            startedUserCacheAcquisitionAttempts: 0,
            completedUserCacheAcquisitionAttempts: 0,
            userCacheWaitTimeMicros: 0,
        },
    };
    hasCommandLogEntry(conn, expectedInsertCommandLog, {}, 1);
    hasNoCommandLogEntry(conn, expectedInsertCommandLog, unexpectedCommandWithoutUserCacheAttrs);

    // Check that there's a log for the successful find command that does NOT contain authorization
    // stats (since it didn't access the user cache).
    let expectedFindCommandLog = {find: "coll"};
    hasCommandLogEntry(conn, expectedFindCommandLog, {}, 2);
    hasNoCommandLogEntry(conn, expectedFindCommandLog, unexpectedCommandWithoutUserCacheAttrs);

    // Check that there's a log for the successful find command that had to access to the user
    // cache.
    hasCommandLogEntry(conn, expectedFindCommandLog, expectedCommandWithUserCacheAttrs);
    checkSlowLogTimeFieldsWithUserCacheAttrs(
        conn, expectedFindCommandLog, expectedCommandWithUserCacheAttrs);

    // Check that there is also a document for the successful find command with authorization stats
    // in system.profile when profiling is enabled on standalones.
    if (mode === 'Standalone') {
        const query = {
            "command.find": "coll",
            "authorization.startedUserCacheAcquisitionAttempts": 1,
            "authorization.completedUserCacheAcquisitionAttempts": 1,
            "authorization.userCacheWaitTimeMicros": {"$gte": 1000000}
        };
        assert.eq(1, testDB.system.profile.find(query).toArray().length);
    }
}

// Standalone
{
    const mongod = MongoRunner.runMongod({auth: ''});
    jsTest.log('Starting user_cache_acquisition_stats.js Standalone');
    runTest(mongod, 'Standalone');
    jsTest.log('SUCCESS user_cache_acquisition_stats.js Standalone');
    MongoRunner.stopMongod(mongod);
}

// Sharded Cluster
{
    const st = new ShardingTest({mongos: [{auth: null}], config: [{auth: null}], shards: 1});
    jsTest.log('Starting user_cache_acquisition_stats.js Sharding');
    runTest(st.s0, 'Sharded');
    jsTest.log('SUCCESS user_cache_acquisition_stats.js Sharding');
    st.stop();
}