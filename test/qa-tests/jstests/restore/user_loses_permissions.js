(function() {

    // Tests running mongorestore with --restoreDbUsersAndRoles and --auth,
    // where the user being used for the restore loses permission to do the
    // restore as part of the restore.
    
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles where'+
            ' the user loses permissions');

    var toolTest = new ToolTest('user_loses_permissons', { auth: '' });
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'user_loses_permissions_dump';

    // the db we'll be using, and the admin db
    var testDB = toolTest.db.getSiblingDB('test');
    var adminDB = toolTest.db.getSiblingDB('admin');

    // create the user admin we'll need
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

    // create the backup user, to do the dump
    adminDB.createUser(
        {
            user: 'backup',
            pwd: 'password',
            roles: [
                { role: 'backup', db: 'admin' },  
            ],
        }
    );

    // for the purposes of the dump, the 'restore' user will not
    // have the privileges to do the restore job
    testDB.createUser(
        {
            user: 'restore',
            pwd: 'password',
            roles: [
                { role: 'read', db: 'test' },
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

    // drop the database
    testDB.dropDatabase();

    // give the restore user the ability to perform the restore
    testDB.grantRolesToUser(
            'restore', 
            [
                { role: 'dbAdmin', db: 'test' },
                { role: 'readWrite', db: 'test' },
            ]
    );

    // run the restore job. it should complete successfully, without the
    // restore user being locked out due to permissions changes
    // TODO: may need to include --drop
    ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
        '--username', 'restore', '--password', 'password', dumpTarget+'/test');
    assert.eq(0, ret);

    // make sure the data was restore correctly
    assert.eq(10, testDB.data.count());
    for (var i = 0; i < 10; i++) {
        assert.eq(1, testDB.data.count({ _id: i }));
    }

    printjson(testDB.getUsers());

    // success
    toolTest.stop(); 

}());
