(function() {

    // Tests running mongorestore with --drop and --restoreDbUsersAndRoles,
    // in addition to --auth, and makes sure the authenticated user does not
    // get dropped before it can complete the restore job.
    
    jsTest.log('Testing dropping the authenticated user with mongorestore');

    var toolTest = new ToolTest('drop_authenticated_user', { auth: '' });
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'drop_authenticated_user_dump';

    // the db we'll be using, and the admin db
    var testDB = toolTest.db.getSiblingDB('test');
    var adminDB = toolTest.db.getSiblingDB('admin');

    // create the users we'll need
    adminDB.createUser(
        {
            user: 'userAdmin',
            pwd: 'password',
            roles: [
                { role: 'userAdminAnyDatabase', db: 'admin' },
                { role: 'readWriteAnyDatabase', db: 'admin' },
            ],
        }
    );
    adminDB.auth('userAdmin', 'password');

    adminDB.createUser(
        {
            user: 'backup',
            pwd: 'password',
            roles: [
                { role: 'backup', db: 'admin' },
            ],
        }       
    );
    testDB.createUser(
        {
            user: 'restore',
            pwd: 'password',
            roles: [
                { role: 'dbAdmin', db: 'test' },
                { role: 'readWrite', db: 'test' },
            ],
        }   
    );

    // create a role
    testDB.createRole(
        {
            role: 'extraRole',
            privileges: [
                { 
                    resource: { db: 'test', collection: '' }, 
                    actions: ['find'],
                },
            ],
            roles: [],
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
            '--dumpDbUsersAndRoles', '--username', 'backup', '--password', 'password',
            '--authenticationDatabase', 'admin');
    assert.eq(0, ret);

    // drop all the data, but not the users or roles
    testDB.data.remove({});
    // sanity check the removal worked
    assert.eq(0, testDB.data.count());

    // insert some data to be removed when --drop is run
    for (var i = 10; i < 20; i++) {
        testDB.data.insert({ _id: i });
    }
    // sanity check the insertion worked
    assert.eq(10, testDB.data.count());

    // restore the data, specifying --drop
    ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
            '--drop', '--username', 'restore', '--password', 'password', 
            dumpTarget+'/test');   
    assert.eq(0, ret);

    // make sure the existing data was removed, and replaced with the dumped data
    assert.eq(10, testDB.data.count());
    for (var i = 0; i < 10; i++) {
        assert.eq(1, testDB.data.count({ _id: i }));
    }

    // make sure the correct roles and users exist
    assert.eq(2, adminDB.getUsers().length);
    assert.eq(1, testDB.getUsers().length);
    assert.eq(1, testDB.getRoles().length);

    // success
    toolTest.stop();

}());
