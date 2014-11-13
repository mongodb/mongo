(function() {

    // Tests using mongorestore to restore only a subset of a dump (either a 
    // single db or a single collection) from a larger dump.
     
    jsTest.log('Testing restoration of a subset of a dump');

    var toolTest = new ToolTest('partial_restore');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'partial_restore_dump';

    // we'll insert data into three collections spread across two dbs
    var dbOne = toolTest.db.getSiblingDB('dbOne');
    var dbTwo = toolTest.db.getSiblingDB('dbTwo');
    var collOne = dbOne.collOne;
    var collTwo = dbOne.collTwo;
    var collThree = dbTwo.collThree;

    // insert a bunch of data
    for (var i = 0; i < 50; i++) {
        collOne.insert({ _id: i+'_collOne' });     
        collTwo.insert({ _id: i+'_collTwo' });
        collThree.insert({ _id: i+'_collThree' });
    }
    // sanity check the insertion worked
    assert.eq(50, collOne.count());
    assert.eq(50, collTwo.count());
    assert.eq(50, collThree.count());

    // dump the data
    var ret = toolTest.runTool('dump', '--out', dumpTarget);
    assert.eq(0, ret);

    // drop the databases
    dbOne.dropDatabase();
    dbTwo.dropDatabase();

    // restore a single db
    ret = toolTest.runTool('restore', '--db', 'dbOne', dumpTarget+'/dbOne');
    assert.eq(0, ret);

    // make sure the restore worked, and nothing else but that db was restored
    assert.eq(50, collOne.count());
    assert.eq(50, collTwo.count());
    assert.eq(0, collThree.count());

    // drop the data 
    dbOne.dropDatabase();

    // restore a single collection
    ret = toolTest.runTool('restore', '--collection', 'collTwo', 
            '--db', 'dbOne', dumpTarget+'/dbOne/collTwo.bson');
    assert.eq(0, ret);

    // make sure the restore worked, and nothing else but that collection was restored
    assert.eq(0, collOne.count());
    assert.eq(50, collTwo.count());
    assert.eq(0, collThree.count());

    // success
    toolTest.stop();
 
}());
