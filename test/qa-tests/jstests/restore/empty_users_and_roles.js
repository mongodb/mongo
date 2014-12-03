(function() {

    if (typeof getToolTest === 'undefined') {
        load('jstests/configs/plain_28.config.js');
    }

    // Tests running mongorestore with --restoreDbUsersAndRoles, with
    // no users or roles in the dump.
    
    jsTest.log('Testing running mongorestore with --restoreDbUsersAndRoles with'+
            ' no users or roles in the dump');

    var toolTest = getToolTest('empty_users_and_roles');
    var commonToolArgs = getCommonToolArguments();

    // run the restore with no users or roles. it should succeed, but create no 
    // users or roles
    var ret = toolTest.runTool.apply(
        toolTest,
        ['restore', '--db', 'test', '--restoreDbUsersAndRoles',
        'jstests/restore/testdata/blankdb'].
            concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // success
    toolTest.stop();

}());
