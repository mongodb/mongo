/**
 * Test that user cache acquisition stats are appropriately
 * updated whenever a UserHandle is acquired via the user cache.
 * @tags: [
 *   requires_replication,
 *   requires_sharding
 * ]
 */

(function() {
'use strict';

load("jstests/libs/parallel_shell_helpers.js");

function hasCommandLogEntry(conn, id, command, attributes, count) {
    let expectedLog = {command: command};
    if (Object.keys(attributes).length > 0) {
        expectedLog = Object.assign({}, expectedLog, attributes);
    }
    checkLog.containsRelaxedJson(conn, id, expectedLog, count);
}

function hasNoCommandLogEntry(conn, id, command, attributes) {
    let expectedLog = {command: command};
    if (Object.keys(attributes).length > 0) {
        expectedLog = Object.assign({}, expectedLog, attributes);
    }
    checkLog.containsRelaxedJson(conn, id, expectedLog, 0);
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
    const logID = 51803;
    let expectedIsMasterCommandLog = {isMaster: 1.0};
    let expectedCommandWithUserCacheAttrs = {
        authorization: {
            startedUserCacheAcquisitionAttempts: 1,
            completedUserCacheAcquisitionAttempts: 1,
            userCacheWaitTimeMicros: waitTimeRegex,
        },
    };
    hasCommandLogEntry(
        conn, logID, expectedIsMasterCommandLog, expectedCommandWithUserCacheAttrs, 1);

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
    hasCommandLogEntry(conn, logID, expectedInsertCommandLog, {}, 1);
    hasNoCommandLogEntry(
        conn, logID, expectedInsertCommandLog, unexpectedCommandWithoutUserCacheAttrs);

    // Check that there's a log for the successful find command that does NOT contain authorization
    // stats (since it didn't access the user cache).
    let expectedFindCommandLog = {find: "coll"};
    hasCommandLogEntry(conn, logID, expectedFindCommandLog, {}, 2);
    hasNoCommandLogEntry(
        conn, logID, expectedFindCommandLog, unexpectedCommandWithoutUserCacheAttrs);

    // Check that there's a log for the successful find command that had to access to the user
    // cache.
    hasCommandLogEntry(conn, logID, expectedFindCommandLog, expectedCommandWithUserCacheAttrs);

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
})();
