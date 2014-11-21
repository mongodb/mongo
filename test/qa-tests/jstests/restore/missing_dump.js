(function() {

    // Tests running mongorestore with a missing dump directory.
    
    jsTest.log('Testing running mongorestore with a missing dump directory');

    var toolTest = new ToolTest('missing_dump');
    toolTest.startDB('foo');

    // run restore with a missing dump directory
    var ret = toolTest.runTool('restore', 'xxxxxxxx');
    assert.neq(0, ret);

    // success
    toolTest.stop();

}());
