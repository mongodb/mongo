(function() {

    // Tests using mongorestore with --restoreDbUsersAndRoles, using a dump from
    // a 2.4 mongod and restoring to a 2.8 mongod, which should fail.
    
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles,'+
        ' restoring a 2.4 dump to a 2.8 mongod');

    var toolTest = new ToolTest('users_and_roles_24_to_28', { binVersion: '2.4' });
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'users_and_roles_24_to_28_dump';

    // the db we'll be using 
    var testDB = toolTest.db.getSiblingDB('test');

    // create some users on the database
    testDB.addUser(
        {
            user: 'userOne',
            pwd: 'pwdOne',
            roles: [
                { role: 'read', db: 'test' },
            ],
        }
    );
    testDB.addUser(
        {
            user: 'userTwo',
            pwd: 'pwdTwo',
            roles: [
                { role: 'readWrite', db: 'test' },
            ],
        }
    );

    // insert some data
    for (var i = 0; i < 10; i++) {
        testDB.data.insert({ _id: i });
    }
    // sanity check the insertion worked
    assert.eq(10, testDB.data.count());

    // dump the data
    var ret = toolTest.runTool('dump', '--out', dumpTarget, '--db', 'test',
                '--dumpDbUsersAndRoles');
    assert.eq(0, ret);

    // drop the database, users, and roles
    testDB.dropDatabase();
    testDB.dropAllUsers();

    // restart the mongod as a 2.8
    stopMongod(toolTest.port);
    toolTest.m = null;
    toolTest.db = null;
    delete toolTest.options.binVersion;
    toolTest.startDB('foo');

    // refresh the db reference
    testDB = toolTest.db.getSiblingDB('test');

    // restore the data, specifying --restoreDBUsersAndRoles. it should fail
    // since the auth version is too old
    ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
                dumpTarget+'/test');
    assert.neq(0, ret);

    // success
    toolTest.stop();

}());
