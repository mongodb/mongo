(function() {

    // Tests using mongorestore with the --oplogReplay and --oplogLimit flags.
    
    jsTest.log('Testing restoration with the --oplogReplay and --oplogLimit options');

    var toolTest = new ToolTest('oplog_replay_and_limit');
    toolTest.startDB('foo');

    // this test uses the testdata/dump_with_oplog directory. this directory contains:
    // - a test/ subdirectory, which will restore objects { _id: i } for i from
    //      0-9 to the test.data collection
    // - an oplog.bson file, which contains oplog entries for inserts of 
    //      objects { _id: i } for i from 10-14 to the test.data collection. 
    //
    // within the oplog.bson file, the entries for i from 10-13 have timestamps
    //      1416342265:2 through 1416342265:5. the entry for { _id: i } has
    //      timestamp 1500000000:1.
    
    // the db and collection we'll be using
    var testDB = toolTest.db.getSiblingDB('test');
    var testColl = testDB.data;

    // restore the data, without --oplogReplay. _ids 0-9, which appear in the
    // collection's bson file, should be restored.
    var ret = toolTest.runTool('restore', 'testdata/dump_with_oplog');
    assert.eq(0, ret);
    assert.eq(10, testColl.count());
    for (var i = 0; i < 10; i++) {
        assert.eq(1, testColl.count({ _id: i }));
    }

    // drop the db
    testDB.dropDatabase();

    // restore the data, with --oplogReplay. _ids 10-14, appearing 
    // in the oplog.bson file, should be inserted as well.
    ret = toolTest.runTool('restore', '--oplogReplay', 'testdata/dump_with_oplog');
    assert.eq(0, ret);
    assert.eq(15, testColl.count());
    for (var i = 0; i < 15; i++) {
        assert.eq(1, testColl.count({ _id: i }));
    }

    // drop the db
    testDB.dropDatabase();

    // restore the data, with --oplogReplay and --oplogLimit with a
    // value that will filter out { _id: 14 } from getting inserted.
    ret = toolTest.runTool('restore', '--oplogReplay', '--oplogLimit',
                '1416342266:0', 'testdata/dump_with_oplog');
    assert.eq(0, ret);
    assert.eq(14, testColl.count());
    for (var i = 0; i < 14; i++) {
        assert.eq(1, testColl.count({ _id: i }));
    }

    // success
    toolTest.stop();

}());
