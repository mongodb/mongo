(function() {

    // Tests using mongorestore on a dump directory containing symlinks
    
    jsTest.log('Testing restoration from a dump containing symlinks');

    var toolTest = new ToolTest('symlinks');
    toolTest.startDB('foo');

    // this test uses the testdata/dump_with_soft_link. within that directory,
    // the dbTwo directory is a soft link to testdata/soft_linked_db and the
    // dbOne/data.bson file is a soft link to testdata/soft_linked_collection.bson
    
    // the two dbs we'll be using
    var dbOne = toolTest.db.getSiblingDB('dbOne'); 
    var dbTwo = toolTest.db.getSiblingDB('dbTwo');

    // restore the data
    var ret = toolTest.runTool('restore', 'testdata/dump_with_soft_links');
    assert.eq(0, ret);

    // make sure the data was restored properly
    assert.eq(10, dbOne.data.count());
    assert.eq(10, dbTwo.data.count());
    for (var i = 0; i < 10; i++) {
        assert.eq(1, dbOne.data.count({ _id: i+'_dbOne' }));
        assert.eq(1, dbTwo.data.count({ _id: i+'_dbTwo' }));
    }

    // success
    toolTest.stop();

}());
