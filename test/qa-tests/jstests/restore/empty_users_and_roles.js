(function() {

    // Tests running mongorestore with --restoreDbUsersAndRoles, with
    // no users or roles in the dump.
    
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles with'+
            ' no users or roles in the dump');

    var toolTest = new ToolTest('empty_users_and_roles');
    toolTest.startDB('foo');

    // run the restore with no users or roles. it should succeed, but create no 
    // users or roles
    var ret = toolTest.runTool('restore', '--db', 'test', '--restoreDbUsersAndRoles',
        'jstests/restore/testdata/blankdb');
    assert.eq(0, ret);

    // success
    toolTest.stop();

}());
