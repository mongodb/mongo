(function() {

    // skip tests requiring wiredTiger storage engine on pre 2.8 mongod
    if (TestData && TestData.storageEngine === 'wiredTiger')
        return

    // Tests using mongorestore with --restoreDbUsersAndRoles, using a dump from
    // a 2.8 mongod and restoring to a 2.6 mongod, which should fail.

    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles,'+
        ' restoring a 2.8 dump to a 2.6 mongod');

    var toolTest = new ToolTest('users_and_roles_28_to_26');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'users_and_roles_28_to_26_dump';
    resetDbpath(dumpTarget);

    // the db we'll be using 
    var testDB = toolTest.db.getSiblingDB('test');

    // create some users and roles on the database
    testDB.createUser(
        {
            user: 'userOne',
            pwd: 'pwdOne',
            roles: [
                { role: 'read', db: 'test' },
            ],
        }
    );
    testDB.createRole(
        {
            role: 'roleOne',
            privileges: [
                { 
                    resource: { db: 'test', collection: '' }, 
                    actions: ['find'],
                },
            ],
            roles: [],
        }
    );
    testDB.createUser(
        {
            user: 'userTwo',
            pwd: 'pwdTwo',
            roles: [
                { role: 'roleOne', db: 'test' },
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
    testDB.dropAllRoles();

    // restart the mongod as a 2.6
    MongoRunner.stopMongod(toolTest.port);
    toolTest.m = null;
    toolTest.db = null;
    toolTest.options = toolTest.options || {};
    toolTest.options.binVersion = '2.6';
    toolTest.startDB('foo');

    // refresh the db reference
    testDB = toolTest.db.getSiblingDB('test');

    // restore the data, specifying --restoreDBUsersAndRoles. it should fail
    // since the auth version is too new
    ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
                dumpTarget+'/test');
    assert.neq(0, ret);

    // success
    toolTest.stop();
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles,'+
        ' restoring a 2.8 dump to a 2.6 mongod');

    var toolTest = new ToolTest('users_and_roles_28_to_26');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'users_and_roles_28_to_26_dump';

    // the db we'll be using 
    var testDB = toolTest.db.getSiblingDB('test');

    // create some users and roles on the database
    testDB.createUser(
        {
            user: 'userOne',
            pwd: 'pwdOne',
            roles: [
                { role: 'read', db: 'test' },
            ],
        }
    );
    testDB.createRole(
        {
            role: 'roleOne',
            privileges: [
                { 
                    resource: { db: 'test', collection: '' }, 
                    actions: ['find'],
                },
            ],
            roles: [],
        }
    );
    testDB.createUser(
        {
            user: 'userTwo',
            pwd: 'pwdTwo',
            roles: [
                { role: 'roleOne', db: 'test' },
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
    testDB.dropAllRoles();

    // restart the mongod as a 2.6
    MongoRunner.stopMongod(toolTest.port);
    toolTest.m = null;
    toolTest.db = null;
    toolTest.options = toolTest.options || {};
    toolTest.options.binVersion = '2.6';
    toolTest.startDB('foo');

    // refresh the db reference
    testDB = toolTest.db.getSiblingDB('test');

    // restore the data, specifying --restoreDBUsersAndRoles. it should fail
    // since the auth version is too new
    ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
                dumpTarget+'/test');
    assert.neq(0, ret);

    // success
    toolTest.stop();

}());
