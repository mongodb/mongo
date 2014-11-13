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

    // run restore with both --collection and --oplogReplay specified
    ret = toolTest.runTool('restore', '--oplogReplay', '--db', 'db_empty', 
            '--collection', 'coll_empty',
            'testdata/dump_empty/db_empty/coll_empty.bson');
    assert.neq(0, ret);

    toolTest.stop();

}());
