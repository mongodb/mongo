(function() {

    // Tests that running mongorestore with --drop on a database with
    // nothing to drop does not error out, and completes the 
    // restore successfully.
    
    jsTest.log('Testing restoration with --drop on a nonexistent db'); 

    var toolTest = new ToolTest('drop_nonexistent_db');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'drop_nonexistent_db_dump';

    // the db we will use 
    var testDB = toolTest.db.getSiblingDB('test');
    
    // insert a bunch of data
    for (var i = 0; i < 500; i++) {
        testDB.coll.insert({ _id: i });
    }
    // sanity check the insertion worked
    assert.eq(500, testDB.coll.count());

    // dump the data
    var ret = toolTest.runTool('dump', '--out', dumpTarget);
    assert.eq(0, ret);

    // drop the database we are using
    testDB.dropDatabase();
    // sanity check the drop worked
    assert.eq(0, testDB.coll.count());

    // restore the data with --drop
    var ret = toolTest.runTool('restore', '--drop', dumpTarget);
    assert.eq(0, ret);

    // make sure the data was restored
    assert.eq(500, testDB.coll.count());
    for (var i = 0; i < 500; i++) {
        assert.eq(1, testDB.coll.count({ _id: i }));
    }

    // success
    toolTest.stop();

}());
