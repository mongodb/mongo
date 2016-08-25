(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests using mongorestore to restore data to multiple dbs.

  jsTest.log('Testing restoration to multiple dbs');

  var toolTest = getToolTest('multiple_dbs');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'multiple_dbs_dump';
  resetDbpath(dumpTarget);

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
    dbOne[oneOnlyCollName].insert({_id: i+'_'+oneOnlyCollName});
    dbTwo[twoOnlyCollName].insert({_id: i+'_'+twoOnlyCollName});
    dbOne[sharedCollName].insert({_id: i+'_dbOne_'+sharedCollName});
    dbTwo[sharedCollName].insert({_id: i+'_dbTwo_'+sharedCollName});
  }
  // sanity check the insertion worked
  assert.eq(50, dbOne[oneOnlyCollName].count());
  assert.eq(50, dbTwo[twoOnlyCollName].count());
  assert.eq(50, dbOne[sharedCollName].count());
  assert.eq(50, dbTwo[sharedCollName].count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the databases
  dbOne.dropDatabase();
  dbTwo.dropDatabase();

  // restore the data
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored properly
  assert.eq(50, dbOne[oneOnlyCollName].count());
  assert.eq(50, dbTwo[twoOnlyCollName].count());
  assert.eq(50, dbOne[sharedCollName].count());
  assert.eq(50, dbTwo[sharedCollName].count());
  for (i = 0; i < 50; i++) {
    assert.eq(1, dbOne[oneOnlyCollName].count({_id: i+'_'+oneOnlyCollName}));
    assert.eq(1, dbTwo[twoOnlyCollName].count({_id: i+'_'+twoOnlyCollName}));
    assert.eq(1, dbOne[sharedCollName].count({_id: i+'_dbOne_'+sharedCollName}));
    assert.eq(1, dbTwo[sharedCollName].count({_id: i+'_dbTwo_'+sharedCollName}));
  }

  // success
  toolTest.stop();

}());
