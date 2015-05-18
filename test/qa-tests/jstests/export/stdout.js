(function() {

    // Tests running mongoexport writing to stdout.
    
    jsTest.log('Testing exporting to stdout');

    var toolTest = new ToolTest('stdout');
    toolTest.startDB('foo');

    // the db and collection we'll use
    var testDB = toolTest.db.getSiblingDB('test');
    var testColl = testDB.data;

    // insert some data
    for (var i = 0; i < 20; i++) {
        testColl.insert({ _id: i });
    }
    // sanity check the insertion worked
    assert.eq(20, testColl.count());

    // export the data, writing to stdout
    var ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data');
    assert.eq(0, ret);

    // grab the raw output
    var output = rawMongoProgramOutput();

    // make sure it contains the json output
    assert.neq(-1, output.indexOf('exported 20 records'));
    for (var i = 0; i < 20; i++) {
        assert.neq(-1, output.indexOf('{"_id":'+i+".0"+'}'));
    }

    // success
    toolTest.stop();

}());
