(function() {

    // Tests using mongorestore to restore data from a collection whose .metadata.json
    // file contains invalid indexes.
    
    jsTest.log('Testing restoration from a metadata file with invalid indexes');

    var toolTest = new ToolTest('invalid_metadata');
    toolTest.startDB('foo');

    // run restore, targeting a collection whose metadata file contains an invalid index
    var ret = toolTest.runTool('restore', '--db', 'dbOne',
            '--collection', 'invalid_metadata',
            'jstests/restore/testdata/dump_with_invalid/dbOne/invalid_metadata.bson');
    assert.neq(0, ret);

    toolTest.stop();

}());
