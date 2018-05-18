// Auth tests for the $listSessions aggregation pipeline.

(function() {
    'use strict';
    load('jstests/aggregation/extras/utils.js');

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    function runListSessionsTest(mongod) {
        assert(mongod);
        const admin = mongod.getDB('admin');
        const config = mongod.getDB('config');

        const pipeline = [{'$listSessions': {}}];
        function listSessions() {
            return config.system.sessions.aggregate(pipeline);
        }

        admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
        assert(admin.auth('admin', 'pass'));

        admin.createUser({user: 'user1', pwd: 'pass', roles: jsTest.basicUserRoles});
        admin.createUser({user: 'user2', pwd: 'pass', roles: jsTest.basicUserRoles});
        admin.logout();

        // Fail when not logged in.
        assertErrorCode(config.system.sessions, pipeline, ErrorCodes.Unauthorized);

        // Start a new session and capture its sessionId.
        assert(admin.auth('user1', 'pass'));
        const myid = assert.commandWorked(admin.runCommand({startSession: 1})).id.id;
        assert(myid !== undefined);

        // Sync cache to collection and ensure it arrived.
        assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
        const resultArray = listSessions().toArray();
        assert.eq(resultArray.length, 1);
        const cacheid = resultArray[0]._id.id;
        assert(cacheid !== undefined);
        assert.eq(bsonWoCompare(cacheid, myid), 0);

        // Ask again using explicit UID.
        const user1Pipeline = [{'$listSessions': {users: [{user: "user1", db: "admin"}]}}];
        function listUser1Sessions() {
            return config.system.sessions.aggregate(user1Pipeline);
        }
        const resultArrayMine = listUser1Sessions().toArray();
        assert.eq(bsonWoCompare(resultArray, resultArrayMine), 0);

        // Make sure pipelining other collections fail
        assertErrorCode(admin.system.collections, pipeline, ErrorCodes.InvalidNamespace);

        // Ensure that changing users hides the session everwhere.
        assert(admin.auth('user2', 'pass'));
        assert.eq(listSessions().toArray().length, 0);

        // Ensure users can't view either other's sessions.
        assertErrorCode(config.system.sessions, user1Pipeline, ErrorCodes.Unauthorized);

        if (true) {
            // TODO SERVER-29141: Support forcing pipelines to run on mongos
            return;
        }
        function listLocalSessions() {
            return config.aggregate([{'$listLocalSessions': {}}]);
        }
        assert.eq(listLocalSessions().toArray().length, 0);
    }

    const mongod = MongoRunner.runMongod({auth: ""});
    runListSessionsTest(mongod);
    MongoRunner.stopMongod(mongod);

    const st =
        new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
    runListSessionsTest(st.s0);
    st.stop();
})();
