(function() {

    // Tests using mongorestore to restore data from a blank db directory.
    
    jsTest.log('Testing restoration from a blank db directory');

    var toolTest = new ToolTest('blank_db');
    toolTest.startDB('foo');

    // run the restore with the blank db directory. it should succeed, but
    // insert nothing.
    var ret = toolTest.runTool('restore', '--db', 'test', 'restore/testdata/blankdb');
    assert.eq(0, ret);

    // success
    toolTest.stop();

}());
