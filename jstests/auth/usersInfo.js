// Test behavior and edge cases in usersInfo
(function() {
    'use strict';

    function runTest(conn) {
        let db = conn.getDB("test");
        let emptyDB = conn.getDB("test2");
        let otherDB = conn.getDB("other");

        const userCount = 200;
        for (let i = 0; i < userCount; ++i) {
            assert.commandWorked(db.runCommand({createUser: "user" + i, pwd: "pwd", roles: []}));
        }
        assert.commandWorked(otherDB.runCommand({createUser: "otherUser", pwd: "pwd", roles: []}));

        // Check info for all users on the "test" database.
        const allTestInfo = assert.commandWorked(db.runCommand({usersInfo: 1}));
        assert.eq(userCount, allTestInfo.users.length);

        // Check we can find a particular user on the "test" database.
        assert.eq(1, assert.commandWorked(db.runCommand({usersInfo: "user12"})).users.length);
        assert.eq(1,
                  assert.commandWorked(db.runCommand({usersInfo: {user: "user12", db: "test"}}))
                      .users.length);
        assert.eq(0,
                  assert.commandWorked(db.runCommand({usersInfo: {user: "user12", db: "test2"}}))
                      .users.length);
        assert.eq(0, assert.commandWorked(emptyDB.runCommand({usersInfo: "user12"})).users.length);

        // No users are found on a database without users.
        assert.eq(0, assert.commandWorked(emptyDB.runCommand({usersInfo: 1})).users.length);

        // Check that we can find records for all users on all databases.
        const allInfo = assert.commandWorked(db.runCommand({usersInfo: {forAllDBs: true}}));
        assert.eq(userCount + 1, allInfo.users.length);
    }

    const m = MongoRunner.runMongod();
    runTest(m);
    MongoRunner.stopMongod(m);

    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    const st =
        new ShardingTest({shards: 1, mongos: 1, config: 1, other: {shardAsReplicaSet: false}});
    runTest(st.s0);
    st.stop();
}());
