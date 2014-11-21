(function() {

    // Tests running mongorestore with bad command line options.

    jsTest.log('Testing running mongorestore with bad'+
            ' command line options');

    var toolTest = new ToolTest('incompatible_flags');
    toolTest.startDB('foo');

    // run restore with both --objcheck and --noobjcheck specified
    var ret = toolTest.runTool('restore', '--objcheck', '--noobjcheck',
            'mongorestore/testdata/dump_empty');
    assert.neq(0, ret);

    // run restore with --oplogLimit with a bad timestamp
    ret = toolTest.runTool('restore', '--oplogReplay', '--oplogLimit', 'xxx',
        'mongorestore/testdata/dump_with_oplog');
    assert.neq(0, ret);

    toolTest.stop();

}());
