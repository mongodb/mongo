(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that running mongorestore with --drop and --collection leaves data
  // in other collections untouched (that --drop only applies to the
  // specified collection).

  jsTest.log('Testing restoration with --drop and --collection, with data in'+
      ' other collections');

  var toolTest = getToolTest('drop_one_collection');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'drop_one_collection_dump';
  resetDbpath(dumpTarget);

  // the db we will take the dump from
  var sourceDB = toolTest.db.getSiblingDB('source');

  // dump from two different collections, even though we'll
  // only be restoring one.
  var collNames = ['coll1', 'coll2'];
  collNames.forEach(function(collName) {
    var data = [];
    for (var i = 0; i < 500; i++) {
      data.push({_id: i+'_'+collName});
    }
    sourceDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(500, sourceDB[collName].count());
  });

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop and replace the data
  collNames.forEach(function(collName) {
    sourceDB[collName].drop();
    // sanity check the drop worked
    assert.eq(0, sourceDB[collName].count());

    // insert a disjoint set of data from the dump
    var data = [];
    for (var i = 500; i < 600; i++) {
      data.push({_id: i+'_'+collName});
    }
    sourceDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(100, sourceDB[collName].count());
  });

  // insert data into the same collections in a different db
  var otherDB = toolTest.db.getSiblingDB('other');
  collNames.forEach(function(collName) {
    var data = [];
    for (var i = 500; i < 600; i++) {
      data.push({_id: i+'_'+collName});
    }
    otherDB[collName].insertMany(data);
    // sanity check the insertion worked
    assert.eq(100, otherDB[collName].count());
  });

  // restore with --drop and --collection
  ret = toolTest.runTool.apply(toolTest, ['restore', '--drop',
      '--db', 'source',
      '--collection', 'coll1']
    .concat(getRestoreTarget(dumpTarget+'/source/coll1.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure that the dumped data replaced the old data in only
  // the specified collection, and all other data was left untouched
  assert.eq(500, sourceDB.coll1.count());
  for (var i = 0; i < 500; i++) {
    assert.eq(1, sourceDB.coll1.count({_id: i+'_coll1'}));
  }
  assert.eq(100, sourceDB.coll2.count());
  assert.eq(100, otherDB.coll1.count());
  assert.eq(100, otherDB.coll2.count());

  // success
  toolTest.stop();

}());
