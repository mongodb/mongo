(function() {

    // Tests using mongorestore to restore data from a malformed bson file.
    
    jsTest.log('Testing restoration from a malformed bson file');

    var toolTest = new ToolTest('malformed_bson');
    toolTest.startDB('foo');

    // run restore, targeting a malformed bson file
    var ret = toolTest.runTool('restore', '--db', 'dbOne', 
            '--collection', 'malformed_coll',
            'jstests/restore/testdata/dump_with_malformed/dbOne/malformed_coll.bson');
    assert.neq(0, ret);

    toolTest.stop();

}());
