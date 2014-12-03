(function() {

    // Tests using mongorestore to restore a dump containing users. If there is
    // conflicting authSchemaVersion in the admin.system.version document, it 
    // should be ignored, and the restore should complete successfully.
    
    jsTest.log('Testing restoring a dump with a potentially conflicting'+
            ' authSchemaVersion in the database');

    var runTest = function(sourceDBVersion, dumpVersion, restoreVersion, destDBVersion) {

        jsTest.log('Running with sourceDBVersion=' + (sourceDBVersion || 'latest') +
                ', dumpVersion=' + (dumpVersion || 'latest') + ', restoreVersion=' +
                (restoreVersion || 'latest') + ', and destDBVersion=' + 
                (destDBVersion || 'latest'));

        var toolTest = new ToolTest('conflicting_auth_schema_version', 
            { binVersion: sourceDBVersion, auth: '' });
        toolTest.startDB('foo');

        // where we'll put the dump
        var dumpTarget = 'conflicting_auth_schema_version_dump';

        // the admin db, and the non-admin db we'll be using
        var adminDB = toolTest.db.getSiblingDB('admin');
        var testDB = toolTest.db.getSiblingDB('test');

        // create a user admin
        adminDB.createUser(
            {
                user: 'admin', 
                pwd: 'password',
                roles: [
                    { role: 'userAdminAnyDatabase', db: 'admin' },
                    { role: 'readWriteAnyDatabase', db: 'admin' },
                    { role: 'backup', db: 'admin' },
                ],
            }
        );
        adminDB.auth('admin', 'password');

        // add some data
        for (var i = 0; i < 10; i++) {
            testDB.data.insert({ _id: i });
        }
        // sanity check the data was inserted
        assert.eq(10, testDB.data.count());

        // dump all the data
        var ret = toolTest.runTool('dump' + (dumpVersion ? ('-'+dumpVersion) : ''),
                '--out', dumpTarget, '--username', 'admin',
                '--password', 'password');
        assert.eq(0, ret);

        // restart the mongod, with a clean db path
        stopMongod(toolTest.port);
        resetDbpath(toolTest.dbpath);
        toolTest.m = null;
        toolTest.db = null;
        toolTest.options.binVersion = destDBVersion;
        toolTest.startDB('foo');

        // refresh the db references
        adminDB = toolTest.db.getSiblingDB('admin');
        testDB = toolTest.db.getSiblingDB('test');

        // create a new user admin
        adminDB.createUser(
            {
                user: 'admin28', 
                pwd: 'password',
                roles: [
                    { role: 'userAdminAnyDatabase', db: 'admin' },
                    { role: 'readWriteAnyDatabase', db: 'admin' },
                    { role: 'restore', db: 'admin' },
                ],
            }
        );
        adminDB.auth('admin28', 'password');

        // do a full restore
        ret = toolTest.runTool('restore' + (restoreVersion ? ('-'+restoreVersion) : ''), 
            '--username', 'admin28', '--password', 
            'password', dumpTarget);
        assert.eq(0, ret);

        // make sure the data and users are all there
        assert.eq(10, testDB.data.count());
        for (var i = 0; i < 10; i++) {
            assert.eq(1, testDB.data.count({ _id: i }));
        }
        var users = adminDB.getUsers();
        assert.eq(2, users.length);
        assert(users[0].user === 'admin' || users[1].user === 'admin');
        assert(users[0].user === 'admin28' || users[1].user === 'admin28');

        // success
        toolTest.stop();

    };

    // 'undefined' triggers latest
    runTest('2.6', '2.6', undefined, '2.6');
    runTest('2.6', '2.6', undefined, undefined);
    runTest('2.6', undefined, undefined, undefined);
    runTest(undefined, '2.6', undefined, '2.6');
    runTest(undefined, undefined, undefined, '2.6');
    runTest(undefined, undefined, undefined, undefined);

}());
