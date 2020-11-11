// Test for invalidation of records across invalidation boundaries.
// @tags: [requires_replication]

(function() {
'use strict';

// Pull in Thread.
load('jstests/libs/parallelTester.js');

function authFailureEvent(log) {
    const kAuthFailureEventID = 20436;
    return log.id === kAuthFailureEventID;
}

function resolveRolesDelayEvent(log) {
    const kResolveRolesDelayID = 4859400;
    return log.id === kResolveRolesDelayID;
}

function invalidateUserEvent(log) {
    const kInvalidateUserID = 20235;
    if (log.id !== kInvalidateUserID) {
        return false;
    }
    const user = log.attr.user;
    return (user.user === 'user1') && (user.db === 'test');
}

function acquireUserEvent(log) {
    const kAcquireUserID = 20238;
    if (log.id !== kAcquireUserID) {
        return false;
    }
    const user = log.attr.user;
    return (user.user === 'user1') && (user.db === 'test');
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
    return ret;
}

/**
 * Negative of assertHasLog() above.
 * Does not return a log line (because there isn't one).
 */
function assertLacksLog(conn, cond, after) {
    const log = checkLog.getGlobalLog(conn);
    log.forEach(function(line) {
        line = JSON.parse(line);
        line.t = Date.parse(line.t['$date']);
        if (line.t <= after) {
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
 * sees 'read' successfully revoked, and our query fails.
 */
function runTest(writeNode, readNode, awaitReplication, lock, unlock) {
    const writeAdmin = writeNode.getDB('admin');
    const readAdmin = readNode.getDB('admin');

    writeAdmin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    assert(writeAdmin.auth('admin', 'pwd'));
    assert.soon(() => readAdmin.auth('admin', 'pwd'));

    const kResolveRolesDelayMS = 5 * 1000;
    assert.commandWorked(readAdmin.runCommand({
        configureFailPoint: 'authLocalGetUser',
        mode: 'alwaysOn',
        data: {resolveRolesDelayMS: NumberInt(kResolveRolesDelayMS)}
    }));
    readNode.setLogLevel(3, 'accessControl');

    const writeTest = writeNode.getDB('test');
    writeTest.createUser({user: 'user1', pwd: 'pwd', roles: ['read']});
    assert.writeOK(writeTest.coll.insert({x: 1}));

    awaitReplication();
    lock();

    jsTest.log('Starting parallel thread');
    const startTime = Date.now();
    const thread = new Thread(function(port) {
        const mongo = new Mongo('localhost:' + port);
        assert(mongo);
        const test = mongo.getDB('test');
        assert(test);

        jsTest.log('Starting auth');
        test.auth('user1', 'pwd');
        jsTest.log('Completed auth');
        // This should fail since read was revoked during mutation.
        assert.throws(() => test.coll.findOne({}));
        jsTest.log('Ran command');
    }, readNode.port);
    thread.start();

    // Wait for initial auth to start.
    jsTest.log('Waiting for auth start');
    const sleep1 = assertHasLog(readNode, resolveRolesDelayEvent, startTime);
    jsTest.log(sleep1);
    // Our initial acquisition has the read role.
    assert.eq(sleep1.attr.directRoles.length, 1);
    assert.eq(sleep1.attr.directRoles[0].role, 'read');
    assert.eq(sleep1.attr.directRoles[0].db, 'test');
    assertLacksLog(readNode, invalidateUserEvent, startTime);

    // Mutate the user to cause an invalidation.
    // Use writeConcern 1 to avoid blocking on the secondary applications.
    jsTest.log('Mutating');
    writeTest.revokeRolesFromUser('user1', ['read'], {w: 1});

    jsTest.log('Looking for invalidation');
    const invalidation = assertHasLog(readNode, invalidateUserEvent, sleep1.t);
    jsTest.log(invalidation);

    jsTest.log('Looking for new acquisiiton');
    const reacquire = assertHasLog(readNode, acquireUserEvent, invalidation.t);
    jsTest.log(reacquire);
    unlock();

    jsTest.log('Looking for second acquire sleep');
    const sleep2 = assertHasLog(readNode, resolveRolesDelayEvent, reacquire.t);
    jsTest.log(sleep2);
    // This acquisition comes from a later snapshot which has no roles.
    assert.eq(sleep2.attr.directRoles.length, 0);

    jsTest.log('Looking for authZ failure for parallel shell write after reacquire');
    const failure = assertHasLog(readNode, authFailureEvent, sleep2.t);
    jsTest.log(failure);

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
