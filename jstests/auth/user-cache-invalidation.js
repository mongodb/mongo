// Test for invalidation of records across invalidation boundaries.
// @tags: [requires_replication]

(function() {
'use strict';

// Pull in Thread.
load('jstests/libs/parallelTester.js');
load("jstests/libs/fail_point_util.js");

const testUser = "user1";
const testDB = "user_cache_invalidation";
const testRole = "read";

function authFailureEvent(log) {
    const kAuthFailureEventID = 20436;
    return log.id === kAuthFailureEventID;
}

function resolveRolesDelayEvent(log) {
    const kResolveRolesDelayID = 5517200;
    if (log.id !== kResolveRolesDelayID) {
        return false;
    }
    const user = log.attr.userName;
    return (user.user === testUser) && (user.db === testDB);
}

function invalidateUserEvent(log) {
    const kInvalidateUserID = 20235;
    if (log.id !== kInvalidateUserID) {
        return false;
    }
    const user = log.attr.user;
    return (user.user === testUser) && (user.db === testDB);
}

function acquireUserEvent(log) {
    const kAcquireUserID = 20238;
    if (log.id !== kAcquireUserID) {
        return false;
    }
    const user = log.attr.user;
    return (user.user === testUser) && (user.db === testDB);
}

/**
 * Check the global log for an entry defined by `cond`
 * occuring after the JS Date `after`.
 *
 * Returns the log line matched with the `t` field
 * transposed to a Javascript Date object.
 */
function assertHasLog(conn, cond, after) {
    var ret = undefined;
    assert.soon(function() {
        const log = checkLog.getGlobalLog(conn);
        var line;
        for (line in log) {
            line = JSON.parse(log[line]);
            if (!cond(line)) {
                continue;
            }

            line.t = Date.parse(line.t['$date']);
            if (line.t >= after) {
                ret = line;
                break;
            }
        }
        return ret !== undefined;
    });

    jsTest.log(`Found log entry: ${tojson(ret)}`);
    return ret;
}

/**
 * Negative of assertHasLog() above.
 * Does not return a log line (because there isn't one).
 */
function assertLacksLog(conn, cond, start, end) {
    const log = checkLog.getGlobalLog(conn);
    log.forEach(function(line) {
        line = JSON.parse(line);
        line.t = Date.parse(line.t['$date']);
        if (line.t < start || line.t > end) {
            return;
        }
        assert(!cond(line), 'Found entry which should not exist: ' + tojson(line));
    });
}

/**
 * Create a user with read permission and simply
 * auth and read in a parallel shell.
 *
 * We use FailPoint 'authLocalGetUser.resolveRolesDelayMS' to
 * give us time to invalidate the user mid-acquisition.
 *
 * We also use pauseBatchApplicationBeforeCompletion with replsets
 * to try to slip the parallel client into the wrong snapshot.
 *
 * When we call revokeRolesFromUser(), this invalidates the
 * user acquisition in progress and forces it to restart.
 *
 * If the restarted acquisition was on the same snapshot,
 * then we'd end up with a stale user being injected into
 * the cache anyway.
 *
 * If the snapshot is advanced, then our parallel shell user
 * sees testRole successfully revoked, and our query fails.
 */
function runTest(writeNode, readNode, awaitReplication, lock, unlock) {
    const writeAdmin = writeNode.getDB('admin');
    const readAdmin = readNode.getDB('admin');

    writeAdmin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    assert(writeAdmin.auth('admin', 'pwd'));

    assert.soon(() => readAdmin.auth('admin', 'pwd'));
    assert.commandWorked(readNode.setLogLevel(3, 'accessControl'));

    const writeTest = writeNode.getDB(testDB);
    writeTest.createUser({user: testUser, pwd: 'pwd', roles: [testRole]});
    assert.writeOK(writeTest.coll.insert({x: 1}));

    awaitReplication();
    lock();

    const startTime = Date.now();
    let currentTime = startTime;
    function assertHasLogAndAdvance(conn, cond) {
        const entry = assertHasLog(conn, cond, currentTime);
        currentTime = entry.t;
        return entry;
    }

    // Set the failpoint before we start the parallel thread.
    const fp = configureFailPoint(
        readNode, 'waitForUserCacheInvalidation', {userName: {db: testDB, user: testUser}});

    // We need some time to mutate the auth state before the acquisition completes.
    const kResolveRolesDelayMS = 5 * 1000;
    assert.commandWorked(readAdmin.runCommand({
        configureFailPoint: 'authLocalGetUser',
        mode: 'alwaysOn',
        data: {resolveRolesDelayMS: NumberInt(kResolveRolesDelayMS)}
    }));

    const thread = new Thread(function(port, testUser, testDB) {
        const mongo = new Mongo('localhost:' + port);
        assert(mongo);
        const test = mongo.getDB(testDB);
        assert(test);

        jsTest.log('Starting auth');
        assert(test.auth(testUser, 'pwd'));
        jsTest.log('Completed auth');

        assert.throws(() => test.coll.findOne({}), [], "Find succeeded despite revokeRoleFromUser");
        jsTest.log('Ran command');
    }, readNode.port, testUser, testDB);
    thread.start();

    // Wait for initial auth to start.
    jsTest.log('Waiting for initial resolve roles');
    {
        const entry = assertHasLogAndAdvance(readNode, resolveRolesDelayEvent);

        // Our initial acquisition has the read role.
        assert.eq(entry.attr.userName.db, testDB);
        assert.eq(entry.attr.userName.user, testUser);
        assert.eq(entry.attr.directRoles.length, 1);
        assert.eq(entry.attr.directRoles[0].role, testRole);
        assert.eq(entry.attr.directRoles[0].db, testDB);
    }
    assertLacksLog(readNode, invalidateUserEvent, startTime, currentTime);

    // Wait for our find to hit the fail point.
    fp.wait();

    // Mutate the user to cause an invalidation.
    // Use writeConcern 1 to avoid blocking on the secondary applications.
    jsTest.log('Mutating');
    writeTest.revokeRolesFromUser(testUser, [testRole], {w: 1});

    jsTest.log('Looking for invalidation');
    assertHasLogAndAdvance(readNode, invalidateUserEvent);

    jsTest.log('Looking for new acquisiiton');
    assertHasLogAndAdvance(readNode, acquireUserEvent);
    unlock();

    jsTest.log('Waiting for second resolve roles');
    assertHasLogAndAdvance(readNode, function(entry) {
        if (!resolveRolesDelayEvent(entry)) {
            return false;
        }

        // This acquisition comes from a later snapshot which has no roles.
        return entry.attr.directRoles.length == 0;
    });

    jsTest.log('Looking for authZ failure for read after revokeRolesFromUser');
    assertHasLogAndAdvance(readNode, authFailureEvent);

    fp.off();
    thread.join();

    jsTest.log('Thread complete');

    writeAdmin.logout();
    readAdmin.logout();
}

{
    // Standalone
    const mongod = MongoRunner.runMongod({auth: ''});
    runTest(mongod, mongod, () => null, () => null, () => null);
    MongoRunner.stopMongod(mongod);
}

{
    // ReplicaSet
    const rst = new ReplSetTest({nodes: 2, keyFile: 'jstests/libs/key1'});
    rst.startSet();
    // Prevent stepdowns, by setting priority to zero on all but one node.
    const cfg = rst.getReplSetConfig();
    for (let i = 0; i < cfg.members.length; ++i) {
        cfg.members[i].priority = i ? 0 : 1;
    }
    rst.initiate(cfg);
    rst.awaitSecondaryNodes();

    // Now identify the permanent primary and secondary we'll use.
    const primary = rst.getPrimary();
    const secondary = rst.getSecondaries()[0];
    const secondaryAdmin = secondary.getDB('admin');

    function lockCompletion() {
        jsTest.log('Enabling pauseBatchApplicationBeforeCompletion on ' + secondary.host);
        assert.commandWorked(secondaryAdmin.runCommand({
            configureFailPoint: 'pauseBatchApplicationBeforeCompletion',
            mode: 'alwaysOn',
        }));
    }

    function unlockCompletion() {
        jsTest.log('Releasing pauseBatchApplicationBeforeCompletion on ' + secondary.host);
        assert.commandWorked(secondaryAdmin.runCommand({
            configureFailPoint: 'pauseBatchApplicationBeforeCompletion',
            mode: 'off',
        }));
    }

    runTest(primary, secondary, () => rst.awaitReplication(), lockCompletion, unlockCompletion);
    rst.stopSet();
}
})();
