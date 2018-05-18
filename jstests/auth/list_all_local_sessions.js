// Auth tests for the $listLocalSessions {allUsers:true} aggregation stage.
// @tags: [requires_sharding]

(function() {
    'use strict';
    load('jstests/aggregation/extras/utils.js');

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    function runListAllLocalSessionsTest(mongod) {
        assert(mongod);
        const admin = mongod.getDB("admin");
        const db = mongod.getDB("test");

        const pipeline = [{'$listLocalSessions': {allUsers: true}}];
        function listAllLocalSessions() {
            return admin.aggregate(pipeline);
        }

        admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
        assert(admin.auth('admin', 'pass'));
        db.createUser({user: 'user1', pwd: 'pass', roles: jsTest.basicUserRoles});
        admin.logout();

        // Shouldn't be able to listLocalSessions when not logged in.
        assertErrorCode(admin, pipeline, ErrorCodes.Unauthorized);

        // Start a new session and capture its sessionId.
        assert(db.auth('user1', 'pass'));
        const myid = assert.commandWorked(db.runCommand({startSession: 1})).id.id;
        assert(myid !== undefined);

        // Ensure that a normal user can NOT listAllLocalSessions to view their session.
        assertErrorCode(admin, pipeline, ErrorCodes.Unauthorized);
        db.logout();

        // Ensure that the cache now contains the session and is visible by admin.
        assert(admin.auth('admin', 'pass'));
        const resultArray = assert.doesNotThrow(listAllLocalSessions).toArray();
        assert.eq(resultArray.length, 1);
        const cacheid = resultArray[0]._id.id;
        assert(cacheid !== undefined);
        assert.eq(0, bsonWoCompare({x: cacheid}, {x: myid}));
    }

    const mongod = MongoRunner.runMongod({auth: ""});
    runListAllLocalSessionsTest(mongod);
    MongoRunner.stopMongod(mongod);

    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}
    });
    runListAllLocalSessionsTest(st.s0);
    st.stop();
})();
