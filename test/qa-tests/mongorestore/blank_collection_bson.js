(function() {

    // Tests using mongorestore to restore data from a blank collection 
    // file, with both a missing and blank metadata file.
    
    jsTest.log('Testing restoration from a blank collection file');

    var toolTest = new ToolTest('blank_collection_bson');
    toolTest.startDB('foo');

    // run the restore with the blank collection file and no
    // metadata file. it should succeed, but insert nothing.
    var ret = toolTest.runTool('restore', '--db', 'test',
            '--collection', 'blank', 'mongorestore/testdata/blankcoll/blank.bson');
    assert.eq(0, ret);
    assert.eq(0, toolTest.db.getSiblingDB('test').blank.count());

    // run the restore with the blank collection file and a blank
    // metadata file. it should succeed, but insert nothing.
    var ret = toolTest.runTool('restore', '--db', 'test',
            '--collection', 'blank', 'mongorestore/testdata/blankcoll/blank_metadata.bson');
    assert.eq(0, ret);
    assert.eq(0, toolTest.db.getSiblingDB('test').blank.count());

    // success
    toolTest.stop();

}());
