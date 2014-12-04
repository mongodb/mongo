(function() {

    if (typeof getToolTest === 'undefined') {
        load('jstests/configs/plain_28.config.js');
    }
    
    // Tests running mongorestore with  --restoreDbUsersAndRoles against 
    // a full dump.
    
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles against'+
        ' a full dump');

    var toolTest = getToolTest('users_and_roles_full_dump');
    var commonToolArgs = getCommonToolArguments();

    // where we'll put the dump
    var dumpTarget = 'users_and_roles_full_dump_dump';

    // the db we'll be using, and the admin db
    var adminDB = toolTest.db.getSiblingDB('admin');
    var testDB = toolTest.db.getSiblingDB('test');

    // create a user and role on the admin database
    adminDB.createUser(
        {
            user: 'adminUser',
            pwd: 'password',
            roles: [
                { role: 'read', db: 'admin' },  
            ],
        }
    );
    adminDB.createRole(
        {
            role: 'adminRole',
            privileges: [
                { 
                    resource: { db: 'admin', collection: '' }, 
                    actions: ['find'],
                },
            ],
            roles: [],
        }
    );

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
    var ret = toolTest.runTool.apply(
        toolTest,    
        ['dump', '--out', dumpTarget].
            concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // drop the database, users, and roles
    testDB.dropDatabase();
    testDB.dropAllUsers();
    testDB.dropAllRoles();

    // drop the admin user and role
    adminDB.dropAllUsers();
    adminDB.dropAllRoles();

    // do a full restore
    ret = toolTest.runTool.apply(
        toolTest,
        ['restore', dumpTarget].
            concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // make sure the data was restored
    assert.eq(10, testDB.data.count());
    for (var i = 0; i < 10; i++) {
        assert.eq(1, testDB.data.count({ _id: i }));
    }

    // make sure the users were restored
    var users = testDB.getUsers();
    assert.eq(2, users.length);
    assert(users[0].user === 'userOne' || users[1].user === 'userOne');
    assert(users[0].user === 'userTwo' || users[1].user === 'userTwo');
    var adminUsers = adminDB.getUsers();
    assert.eq(1, adminUsers.length);
    assert.eq('adminUser', adminUsers[0].user);

    // make sure the roles were restored
    var roles = testDB.getRoles();
    assert.eq(1, roles.length);
    assert.eq('roleOne', roles[0].role);
    var adminRoles = adminDB.getRoles();
    assert.eq(1, adminRoles.length);
    assert.eq('adminRole', adminRoles[0].role);

    // success
    toolTest.stop();
    

}());
