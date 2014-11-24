(function() {

    // Tests running mongorestore with a missing dump files and directories.
    
    jsTest.log('Testing running mongorestore with missing dump files and directories');

    var toolTest = new ToolTest('missing_dump');
    toolTest.startDB('foo');

    // run restore with a missing dump directory
    var ret = toolTest.runTool('restore', 'xxxxxxxx');
    assert.neq(0, ret);

    // run restore with --db and a missing dump directory
    ret = toolTest.runTool('restore', '--db', 'test', 'xxxxxxxx');
    assert.neq(0, ret);

    // specify --collection with a missing file
    ret = toolTest.runTool('restore', '--db', 'test', '--collection', 'data', 
        'jstests/restore/testdata/blankdb/xxxxxxxx.bson');
    assert.neq(0, ret);

    // success
    toolTest.stop();

}());
