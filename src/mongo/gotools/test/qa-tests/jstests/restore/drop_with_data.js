(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that running mongorestore with --drop drops existing data
  // before restoring.

  jsTest.log('Testing restoration with --drop on existing data');

  var toolTest = getToolTest('drop_with_data');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'drop_with_data_dump';
  resetDbpath(dumpTarget);

  // the db we will use
  var testDB = toolTest.db.getSiblingDB('test');

  // we'll use two collections, to make sure they both
  // get dropped appropriately
  var collNames = ['coll1', 'coll2'];

  // insert a bunch of data to be dumped
  collNames.forEach(function(collName) {
    var data = [];
    for (var i = 0; i < 500; i++) {
      data.push({_id: i+'_'+collName});
    }
    testDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(500, testDB[collName].count());
  });

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop all the data, and replace it with different data
  collNames.forEach(function(collName) {
    testDB[collName].drop();
    // sanity check the drop worked
    assert.eq(0, testDB[collName].count());

    var data = [];
    for (var i = 500; i < 600; i++) {
      data.push({_id: i+'_'+collName});
    }
    testDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(100, testDB[collName].count());
  });

  // restore with --drop. the current data in all collections should
  // be removed and replaced with the dumped data
  ret = toolTest.runTool.apply(toolTest, ['restore', '--drop']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the dumped data was restored, and the old data
  // was dropped
  collNames.forEach(function(collName) {
    assert.eq(500, testDB[collName].count());
    for (var i = 0; i < 500; i++) {
      assert.eq(1, testDB[collName].count({_id: i+'_'+collName}));
    }
  });

  // success
  toolTest.stop();

}());
