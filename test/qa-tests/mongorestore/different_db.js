(function() {

    // Tests using mongorestore to restore data to a different db than
    // it was dumped from.

    jsTest.log('Testing restoration to a different db');

    var toolTest = new ToolTest('different_db');
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'different_db_dump';

    // the db we will dump from 
    var sourceDB = toolTest.db.getSiblingDB('source');
    // the db we will restore to
    var destDB = toolTest.db.getSiblingDB('dest');

    // we'll use two collections
    var collNames = ['coll1', 'coll2'];

    // insert a bunch of data
    collNames.forEach(function(collName) {
        for (var i = 0; i < 500; i++) {
            sourceDB[collName].insert({ _id: i+'_'+collName });
        };
        // sanity check the insertion worked
        assert.eq(500, sourceDB[collName].count());
    });

    // dump the data 
    var ret = toolTest.runTool('dump', '--out', dumpTarget);
    assert.eq(0, ret);

    // restore the data to a different db
    ret = toolTest.runTool('restore', '--db', 'dest', dumpTarget+'/source');
    assert.eq(0, ret);

    // make sure the data was restored
    collNames.forEach(function(collName) {
        assert.eq(500, destDB[collName].count());
        for (var i = 0; i < 500; i++) {
            assert.eq(1, destDB[collName].count({ _id: i+'_'+collName }));
        }
    });

    // success
    toolTest.stop();

})();
