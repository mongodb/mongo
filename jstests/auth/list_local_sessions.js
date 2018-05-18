// All tests for the $listLocalSessions aggregation stage.

(function() {
    'use strict';
    load('jstests/aggregation/extras/utils.js');

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    function runListLocalSessionsTest(mongod) {
        assert(mongod);
        const admin = mongod.getDB('admin');
        const db = mongod.getDB("test");

        const pipeline = [{'$listLocalSessions': {}}];
        function listLocalSessions() {
            return admin.aggregate(pipeline);
        }

        admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
        assert(admin.auth('admin', 'pass'));

        db.createUser({user: 'user1', pwd: 'pass', roles: jsTest.basicUserRoles});
        db.createUser({user: 'user2', pwd: 'pass', roles: jsTest.basicUserRoles});
        admin.logout();

        // Shouldn't be able to listLocalSessions when not logged in.
        assertErrorCode(admin, pipeline, ErrorCodes.Unauthorized);

        // Start a new session and capture its sessionId.
        assert(db.auth('user1', 'pass'));
        const myid = assert.commandWorked(db.runCommand({startSession: 1})).id.id;
        assert(myid !== undefined);

        // Ensure that the cache now contains the session.
        const resultArray = assert.doesNotThrow(listLocalSessions).toArray();
        assert.eq(resultArray.length, 1);
        const cacheid = resultArray[0]._id.id;
        const myuid = resultArray[0]._id.uid;
        assert(cacheid !== undefined);
        assert.eq(0, bsonWoCompare({x: cacheid}, {x: myid}));

        // Try asking for the session by username.
        function listMyLocalSessions() {
            return admin.aggregate(
                [{'$listLocalSessions': {users: [{user: "user1", db: "test"}]}}]);
        }
        const resultArrayMine = assert.doesNotThrow(listMyLocalSessions).toArray();
        assert.eq(bsonWoCompare(resultArray, resultArrayMine), 0);

        // Ensure that changing users hides the session.
        assert(db.auth('user2', 'pass'));
        const otherArray = assert.doesNotThrow(listLocalSessions).toArray();
        assert.eq(otherArray.length, 0);

        // Ensure that one user can not explicitly ask for another's sessions.
        assertErrorCode(admin,
                        [{'$listLocalSessions': {users: [{user: "user1", db: "test"}]}}],
                        ErrorCodes.Unauthorized);
    }

    const mongod = MongoRunner.runMongod({auth: ""});
    runListLocalSessionsTest(mongod);
    MongoRunner.stopMongod(mongod);

    const st =
        new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
    runListLocalSessionsTest(st.s0);
    st.stop();
})();
