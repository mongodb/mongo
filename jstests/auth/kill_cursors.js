// Test the killCursors command.
// @tags: [requires_sharding]
(function() {
    'use strict';

    // TODO SERVER-35447: Multiple users cannot be authenticated on one connection within a session.
    TestData.disableImplicitSessions = true;

    function runTest(mongod) {
        /**
         * Open a cursor on `db` while authenticated as `authUsers`.
         * Then logout, and log back in as `killUsers` and try to kill that cursor.
         *
         * @param db - The db to create a cursor on and ultimately kill agains.
         * @param authUsers - Array of ['username', db] pairs to create the cursor under.
         * @param killUsers - Array of ['username', dn] pairs to use when killing.
         * @param shouldWork - Whether we expect success
         */
        function tryKill(db, authUsers, killUsers, shouldWork) {
            function loginAll(users) {
                users.forEach(function(u) {
                    assert(u[1].auth(u[0], 'pass'));
                });
            }

            function logoutAll() {
                [testA, testB].forEach(function(d) {
                    const users = assert.commandWorked(d.runCommand({connectionStatus: 1}))
                                      .authInfo.authenticatedUsers;
                    users.forEach(function(u) {
                        mongod.getDB(u.db).logout();
                    });
                });
            }

            function doKill(extra) {
                // Create a cursor to be killed later.
                loginAll(authUsers);
                let cmd = {find: db.coll.getName(), batchSize: 2};
                Object.assign(cmd, extra);
                const id = assert.commandWorked(db.runCommand(cmd)).cursor.id;
                assert.neq(id, 0, "Invalid cursor ID");
                logoutAll();

                loginAll(killUsers);
                const killCmd = db.runCommand({killCursors: db.coll.getName(), cursors: [id]});
                logoutAll();
                if (shouldWork) {
                    assert.commandWorked(killCmd, "Unable to kill cursor");
                } else {
                    assert.commandFailed(killCmd, "Should not have been able to kill cursor");
                }
            }

            doKill({});
            if ((authUsers.length === 1) && (killUsers.length === 1)) {
                // Session variant only makes sense with single auth'd users.
                doKill({lsid: {id: BinData(4, "QlLfPHTySm6tqfuV+EOsVA==")}});
            }
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
        admin.logout();

        // Create a collection with batchable data
        assert(testA.auth('user1', 'pass'));
        assert(testB.auth('user3', 'pass'));
        for (var i = 0; i < 101; ++i) {
            assert.writeOK(testA.coll.insert({_id: i}));
            assert.writeOK(testB.coll.insert({_id: i}));
        }
        testA.logout();
        testB.logout();

        // A user can kill their own cursor.
        tryKill(testA, [['user1', testA]], [['user1', testA]], true);
        tryKill(testA, [['user2', testA]], [['user2', testA]], true);
        tryKill(testB, [['user3', testB]], [['user3', testB]], true);
        tryKill(testB, [['user4', testB]], [['user4', testB]], true);

        // A user cannot kill someone else's cursor.
        tryKill(testA, [['user1', testA]], [['user2', testA]], false);
        tryKill(testA, [['user1', testA]], [['user2', testA], ['user3', testB]], false);
        tryKill(testA, [['user2', testA]], [['user1', testA]], false);
        tryKill(testA, [['user2', testA]], [['user1', testA], ['user3', testB]], false);
        tryKill(testB, [['user3', testB]], [['user1', testA], ['user4', testB]], false);
        tryKill(testB, [['user3', testB]], [['user2', testA], ['user4', testB]], false);

        // A multi-owned cursor can be killed by any/all owner.
        tryKill(testA, [['user1', testA], ['user3', testB]], [['user1', testA]], true);
        tryKill(testB, [['user1', testA], ['user3', testB]], [['user3', testB]], true);
        tryKill(testA,
                [['user1', testA], ['user3', testB]],
                [['user1', testA], ['user3', testB]],
                true);
        tryKill(testA,
                [['user1', testA], ['user3', testB]],
                [['user2', testA], ['user3', testB]],
                true);
        tryKill(testB,
                [['user1', testA], ['user3', testB]],
                [['user1', testA], ['user3', testB]],
                true);
        tryKill(testB,
                [['user1', testA], ['user3', testB]],
                [['user1', testA], ['user4', testB]],
                true);

        // An owned cursor can not be killed by other user(s).
        tryKill(testA,
                [['user1', testA], ['user3', testB]],
                [['user2', testA], ['user4', testB]],
                false);
        tryKill(testA, [['user1', testA]], [['user2', testA], ['user3', testB]], false);
        tryKill(testA,
                [['user1', testA], ['user3', testB]],
                [['user2', testA], ['user4', testB]],
                false);

        // Admin can kill anything.
        tryKill(testA, [['user1', testA]], [['admin', admin]], true);
        tryKill(testA, [['user2', testA]], [['admin', admin]], true);
        tryKill(testB, [['user3', testB]], [['admin', admin]], true);
        tryKill(testB, [['user4', testB]], [['admin', admin]], true);
        tryKill(testA, [['user1', testA], ['user3', testB]], [['admin', admin]], true);
        tryKill(testB, [['user2', testA], ['user4', testB]], [['admin', admin]], true);
    }

    const mongod = MongoRunner.runMongod({auth: ""});
    runTest(mongod);
    MongoRunner.stopMongod(mongod);

    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}
    });
    runTest(st.s0);
    st.stop();
})();
