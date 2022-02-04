// Test the killCursors command.
// @tags: [requires_sharding]
(function() {
'use strict';

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

function runTest(mongod) {
    /**
     * Open a cursor on `db` while authenticated as `authUsers`.
     * Then logout, and log back in as `killUsers` and try to kill that cursor.
     *
     * @param db - The db to create a cursor on and ultimately kill agains.
     * @param authUsers - Array of ['username', db] pairs to create the cursor under.
     * @param killUsers - Array of ['username', db] pairs to use when killing.
     * @param shouldWork - Whether we expect success
     */
    function tryKill(db, authUser, killUsers, shouldWork) {
        function doKill(extra) {
            // Create a cursor to be killed later.
            assert(authUser[1].auth(authUser[0], 'pass'));
            const cmd = Object.assign({find: db.coll.getName(), batchSize: 2}, extra);
            const id = assert.commandWorked(db.runCommand(cmd)).cursor.id;
            assert.neq(id, 0, "Invalid cursor ID");
            authUser[1].logout();

            killUsers.forEach(function(killUser) {
                assert(killUser[1].auth(killUser[0], 'pass'));
                const cmd = db.runCommand({killCursors: db.coll.getName(), cursors: [id]});
                killUser[1].logout();

                if (shouldWork) {
                    assert.commandWorked(cmd, "Unable to kill cursor");
                } else {
                    assert.commandFailed(cmd, "Should not have been able to kill cursor");
                }
            });
        }

        // Run though create/kill with and without a session ID.
        doKill({});
        doKill({lsid: {id: BinData(4, "QlLfPHTySm6tqfuV+EOsVA==")}});
    }

    function trySelfKill(user) {
        const db = user[1];
        assert(db.auth(user[0], 'pass'));

        assert.commandWorked(db.runCommand({startSession: 1}));

        const cmd = {aggregate: 1, pipeline: [{$listLocalSessions: {}}], cursor: {batchSize: 0}};
        const res = assert.commandWorked(db.runCommand(cmd));
        print(tojson(res));
        const id = res.cursor.id;
        assert.neq(id, 0, "Invalid cursor ID");

        const killCmdRes = db.runCommand({killCursors: db.getName() + ".$cmd", cursors: [id]});
        db.logout();

        assert.commandWorked(killCmdRes, "Unable to kill cursor");
    }

    /**
     * Create user1/user2 in testA, and user3/user4 in testB.
     * Create two 101 element collections in testA and testB.
     * Use various combinations of those users to open cursors,
     * then (potentially) different combinations of users to kill them.
     *
     * A cursor should only be killable if at least one of the users
     * who created it is trying to kill it.
     */

    const testA = mongod.getDB('testA');
    const testB = mongod.getDB('testB');
    const admin = mongod.getDB('admin');

    // Setup users
    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    testA.createUser({user: 'user1', pwd: 'pass', roles: jsTest.basicUserRoles});
    testA.createUser({user: 'user2', pwd: 'pass', roles: jsTest.basicUserRoles});
    testB.createUser({user: 'user3', pwd: 'pass', roles: jsTest.basicUserRoles});
    testB.createUser({user: 'user4', pwd: 'pass', roles: jsTest.basicUserRoles});
    testB.createUser({user: 'user5', pwd: 'pass', roles: []});
    admin.logout();

    // Create a collection with batchable data
    assert(testA.auth('user1', 'pass'));
    for (let i = 0; i < 101; ++i) {
        assert.commandWorked(testA.coll.insert({_id: i}));
    }
    testA.logout();

    assert(testB.auth('user3', 'pass'));
    for (let i = 0; i < 101; ++i) {
        assert.commandWorked(testB.coll.insert({_id: i}));
    }
    testB.logout();

    // A user can kill their own cursor.
    tryKill(testA, ['user1', testA], [['user1', testA]], true);
    tryKill(testA, ['user2', testA], [['user2', testA]], true);
    tryKill(testB, ['user3', testB], [['user3', testB]], true);
    tryKill(testB, ['user4', testB], [['user4', testB]], true);
    trySelfKill(['user1', testA]);
    trySelfKill(['user5', testB]);
    trySelfKill(['admin', admin]);

    // A user cannot kill someone else's cursor.
    tryKill(testA, ['user1', testA], [['user2', testA]], false);
    tryKill(testA, ['user1', testA], [['user2', testA], ['user3', testB]], false);
    tryKill(testA, ['user2', testA], [['user1', testA]], false);
    tryKill(testA, ['user2', testA], [['user1', testA], ['user3', testB]], false);
    tryKill(testB, ['user3', testB], [['user1', testA], ['user4', testB]], false);
    tryKill(testB, ['user3', testB], [['user2', testA], ['user4', testB]], false);

    // Admin can kill anything.
    tryKill(testA, ['user1', testA], [['admin', admin]], true);
    tryKill(testA, ['user2', testA], [['admin', admin]], true);
    tryKill(testB, ['user3', testB], [['admin', admin]], true);
    tryKill(testB, ['user4', testB], [['admin', admin]], true);
}

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();
})();
