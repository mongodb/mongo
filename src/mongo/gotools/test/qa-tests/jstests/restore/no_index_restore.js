(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that running mongorestore with --noIndexRestore does not
  // restore indexes.

  jsTest.log('Testing restoration with --noIndexRestore');

  var toolTest = getToolTest('no_index_restore');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'no_index_restore_dump';
  resetDbpath(dumpTarget);

  // the db we will use
  var testDB = toolTest.db.getSiblingDB('test');

  // we'll use two collections, one with no indexes, the other
  // with indexes
  var collNames = ['coll1', 'coll2'];

  // insert some data to be dumped
  collNames.forEach(function(collName) {
    for (var i = 0; i < 10; i++) {
      testDB[collName].insert({_id: i, num: i+1, s: ''+i});
    }
    // sanity check the insertion worked
    assert.eq(10, testDB[collName].count());
  });

  // create some indexes for the second collection
  testDB.coll2.ensureIndex({num: 1});
  testDB.coll2.ensureIndex({num: 1, s: -1});
  // sanity check the indexes were created
  assert.eq(3, testDB.coll2.getIndexes().length);

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the collections
  collNames.forEach(function(collName) {
    testDB[collName].drop();
    // sanity check the drop worked
    assert.eq(0, testDB[collName].count());
    assert.eq(0, testDB[collName].getIndexes().length);
  });

  // restore the data, with --noIndexRestore
  ret = toolTest.runTool.apply(toolTest, ['restore', '--noIndexRestore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored fully, and only the _id
  // indexes were restored
  collNames.forEach(function(collName) {
    assert.eq(10, testDB[collName].count());
    for (var i = 0; i < 10; i++) {
      assert.eq(1, testDB[collName].count({_id: i}));
    }

    assert.eq(1, testDB[collName].getIndexes().length);
  });

  // success
  toolTest.stop();

}());
