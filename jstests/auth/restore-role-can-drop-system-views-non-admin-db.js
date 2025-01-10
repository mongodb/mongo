// Validate that the restore role is able to drop <db>.system.views for any db
// @tags: [requires_sharding, requires_auth]

function runTest(conn, isSharding) {
    const admin = conn.getDB('admin');
    const test = conn.getDB('test');

    // Setup users
    admin.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
    assert(admin.auth('admin', 'admin'));
    admin.createUser({user: 'restore', pwd: 'restore', roles: ['restore']});

    // Setup some objects include simple, passthrough views.
    assert.commandWorked(test.mycoll.insert({x: 1}));
    assert.commandWorked(test.createView("myView", "mycoll", []));
    assert.commandWorked(admin.createView("usersView", "system.users", []));

    // Test the drops.
    assert.eq(test.myView.count(), 1);
    assert.eq(admin.usersView.count(), 2);

    {
        const uri = `mongodb://restore:restore@${conn.host}/admin`;
        const testConn = new Mongo(uri);
        assert(testConn.getDB('test').myView.drop());
        // Dropping views on admin or config is prohibited on mongos.
        assert(isSharding || testConn.getDB('admin').usersView.drop());
    }

    assert.eq(test.myView.count(), 0);
    assert.eq(admin.usersView.count(), isSharding ? 2 : 0);
}

{
    const standalone = MongoRunner.runMongod({auth: ''});
    runTest(standalone, false);
    MongoRunner.stopMongod(standalone);
}

{
    const st =
        new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
    runTest(st.s0, true);
    st.stop();
}
