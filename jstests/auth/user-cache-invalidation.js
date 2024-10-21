// Test for invalidation of records across invalidation boundaries.
// @tags: [requires_replication,does_not_support_stepdowns,requires_fcv_81]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testUser = "user1";
const testDB = "user_cache_invalidation";
const testRole = "read";

function isAuthFailureEvent(log) {
    const kAuthFailureEventID = 20436;
    return log.id === kAuthFailureEventID;
}

function isEndAcquireUserEvent(log) {
    const kResolveRolesDelayID = 5517200;
    if (log.id !== kResolveRolesDelayID) {
        return false;
    }
    const user = log.attr.userName;
    return (user.user === testUser) && (user.db === testDB);
}

function isInvalidateUserEvent(log) {
    const kInvalidateUserID = 20235;
    if (log.id !== kInvalidateUserID) {
        return false;
    }
    const user = log.attr.user;
    return (user.user === testUser) && (user.db === testDB);
}

function isStartAcquireUserEvent(log) {
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
 * We use FailPoint 'waitForUserCacheInvalidation' to
 * pause so that we can invalidate the user mid-acquisition.
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
function runTest(writeNode, readNode, awaitReplication) {
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

    const startTime = Date.now();
    let currentTime = startTime;
    function assertHasLogAndAdvance(conn, cond) {
        const entry = assertHasLog(conn, cond, currentTime);
        currentTime = entry.t;
        return entry;
    }

    // Set the failpoint before we start the parallel thread so that findOne blocks before user
    // acquisition.
    const fp = configureFailPoint(
        readNode, 'waitForUserCacheInvalidation', {userName: {db: testDB, user: testUser}});

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
    jsTest.log('Looking for initial user acquisition');
    assertHasLogAndAdvance(readNode, isStartAcquireUserEvent);
    jsTest.log('Waiting for initial acquisition to end');
    {
        const entry = assertHasLogAndAdvance(readNode, isEndAcquireUserEvent);

        // Our initial acquisition has the read role.
        assert.eq(entry.attr.userName.db, testDB);
        assert.eq(entry.attr.userName.user, testUser);
        assert.eq(entry.attr.directRoles.length, 1);
        assert.eq(entry.attr.directRoles[0].role, testRole);
        assert.eq(entry.attr.directRoles[0].db, testDB);
    }
    assertLacksLog(readNode, isInvalidateUserEvent, startTime, currentTime);

    // Wait for our find to hit the fail point.
    fp.wait();

    // Mutate the user to cause an invalidation.
    // Use writeConcern 1 to avoid blocking on the secondary applications.
    jsTest.log('Mutating');
    writeTest.revokeRolesFromUser(testUser, [testRole], {w: 1});

    jsTest.log('Looking for invalidation');
    assertHasLogAndAdvance(readNode, isInvalidateUserEvent);

    // Once invalidation happens, the parallel thread breaks out from the
    // waitForUserCacheInvalidation failpoint and proceeds to acquire the user object.
    jsTest.log('Looking for new acquisition');
    assertHasLogAndAdvance(readNode, isStartAcquireUserEvent);

    // This should result in getting the newly updated user object without the removed role.
    jsTest.log('Waiting for reacquisition to end');
    assertHasLogAndAdvance(readNode, function(entry) {
        if (!isEndAcquireUserEvent(entry)) {
            return false;
        }

        // This acquisition comes from a later snapshot which has no roles.
        return entry.attr.directRoles.length == 0;
    });

    jsTest.log('Looking for authZ failure for read after revokeRolesFromUser');
    assertHasLogAndAdvance(readNode, isAuthFailureEvent);

    fp.off();
    thread.join();

    jsTest.log('Thread complete');

    writeAdmin.logout();
    readAdmin.logout();
}

{
    // Standalone
    const mongod = MongoRunner.runMongod({auth: ''});
    runTest(mongod, mongod, () => null);
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

    // Freeze secondaries to avoid surprise stepdowns.
    rst.getSecondaries().forEach(node => rst.freeze(node));
    rst.awaitReplication();

    // Now identify the permanent primary and secondary we'll use.
    const primary = rst.getPrimary();
    const secondary = rst.getSecondaries()[0];

    runTest(primary, secondary, () => rst.awaitReplication());
    rst.stopSet();
}
