(function() {

    // Tests using mongorestore to restore data to multiple dbs.
    
    jsTest.log('Testing restoration to multiple dbs'); 

    var toolTest = new ToolTest('multiple_dbs');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'multiple_dbs_dump';

    // the dbs we will be using
    var dbOne = toolTest.db.getSiblingDB('dbOne');
    var dbTwo = toolTest.db.getSiblingDB('dbTwo');

    // we'll use two collections in each db, with one of
    // the collection names common across the dbs
    var oneOnlyCollName = 'dbOneColl';
    var twoOnlyCollName = 'dbTwoColl';
    var sharedCollName = 'bothColl';

    // insert a bunch of data
    for (var i = 0; i < 50; i++) {
        dbOne[oneOnlyCollName].insert({ _id: i+'_'+oneOnlyCollName });
        dbTwo[twoOnlyCollName].insert({ _id: i+'_'+twoOnlyCollName });
        dbOne[sharedCollName].insert({ _id: i+'_dbOne_'+sharedCollName });
        dbTwo[sharedCollName].insert({ _id: i+'_dbTwo_'+sharedCollName });
    }
    // sanity check the insertion worked
    assert.eq(50, dbOne[oneOnlyCollName].count());
    assert.eq(50, dbTwo[twoOnlyCollName].count());
    assert.eq(50, dbOne[sharedCollName].count());
    assert.eq(50, dbTwo[sharedCollName].count());

    // dump the data
    var ret = toolTest.runTool('dump', '--out', dumpTarget);
    assert.eq(0, ret);

    // drop the databases
    dbOne.dropDatabase();
    dbTwo.dropDatabase();

    // restore the data
    ret = toolTest.runTool('restore', dumpTarget);
    assert.eq(0, ret);

    // make sure the data was restored properly
    assert.eq(50, dbOne[oneOnlyCollName].count());
    assert.eq(50, dbTwo[twoOnlyCollName].count());
    assert.eq(50, dbOne[sharedCollName].count());
    assert.eq(50, dbTwo[sharedCollName].count());
    for (var i = 0; i < 50; i++) {
        assert.eq(1, dbOne[oneOnlyCollName].count({ _id: i+'_'+oneOnlyCollName }));
        assert.eq(1, dbTwo[twoOnlyCollName].count({ _id: i+'_'+twoOnlyCollName }));
        assert.eq(1, dbOne[sharedCollName].count({ _id: i+'_dbOne_'+sharedCollName }));
        assert.eq(1, dbTwo[sharedCollName].count({ _id: i+'_dbTwo_'+sharedCollName }));
    }

    // success
    toolTest.stop();

}());
