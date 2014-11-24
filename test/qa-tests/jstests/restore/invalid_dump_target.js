(function() {

    // Tests running mongorestore with invalid specified dumps (directories when
    // files are expected, and visa versa).
    
    jsTest.log('Testing running mongorestore with a invalid dump targets');

    var toolTest = new ToolTest('invalid_dump_target');
    toolTest.startDB('foo');

    // run restore with a file, not a directory, specified as the dump location
    var ret = toolTest.runTool('restore', 'jstests/restore/testdata/blankdb/README');
    assert.neq(0, ret);

    // run restore with --db specified and a file, not a directory, as the db dump
    ret = toolTest.runTool('restore', '--db', 'test',
        'jstests/restore/testdata/blankdb/README');
    assert.neq(0, ret);

    // run restore with --collection specified and a directory, not a file, 
    // as the dump file
    ret = toolTest.runTool('restore', '--db', 'test', '--collection', 'blank',
        'jstests/restore/testdata/blankdb');
    assert.neq(0, ret);

    // success
    toolTest.stop();

}());
