(function() {

    // Tests running mongorestore with incompatible command line options.

    jsTest.log('Testing running mongorestore with incompatible'+
            ' command line options');

    var toolTest = new ToolTest('incompatible_flags');
    toolTest.startDB('foo');

    // run restore with both --objcheck and --noobjcheck specified
    var ret = toolTest.runTool('restore', '--objcheck', '--noobjcheck',
            'testdata/dump_empty');
    assert.neq(0, ret);

    toolTest.stop();

}());
