(function() {

    if (typeof getToolTest === 'undefined') {
        load('jstests/configs/plain_28.config.js');
    }

    // Tests using mongorestore to restore data from a blank db directory.
    
    jsTest.log('Testing restoration from a blank db directory');

    var toolTest = getToolTest('blank_db');
    var commonToolArgs = getCommonToolArguments();

    // run the restore with the blank db directory. it should succeed, but
    // insert nothing.
    var ret = toolTest.runTool.apply(
        toolTest,
        ['restore', '--db', 'test', 'jstests/restore/testdata/blankdb'].
            concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // success
    toolTest.stop();

}());
