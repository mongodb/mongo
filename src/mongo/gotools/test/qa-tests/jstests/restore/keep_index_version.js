(function() {

  load('jstests/common/check_version.js');

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that running mongorestore with --keepIndexVersion does not
  // update the index version, and that running it without
  // --keepIndexVersion does.

  jsTest.log('Testing mongorestore with --keepIndexVersion');

  var toolTest = getToolTest('keep_index_version');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'keep_index_version_dump';
  resetDbpath(dumpTarget);

  // the db and collection we will use
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.coll;

  if (isAtLeastVersion(testDB.version(), '3.1.0')) {
    jsTest.log("skipping test on "+testDB.version());
    return;
  }
  if (TestData && TestData.storageEngine === 'wiredTiger') {
    jsTest.log("skipping test on "+testDB.version()+" when storage engine is wiredTiger");
    return;
  }

  // create a version 0 index on the collection
  testColl.ensureIndex({num: 1}, {v: 0});

  // insert some data
  for (var i = 0; i < 10; i++) {
    testColl.insert({num: i});
  }
  // sanity check the insert worked
  assert.eq(10, testColl.count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the db
  testDB.dropDatabase();

  // restore the data
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  assert.eq(10, testColl.count());

  // make sure the index version was updated
  var indexes = testColl.getIndexes();
  assert.eq(2, indexes.length);
  assert.eq(1, indexes[1].v);

  // drop the db
  testDB.dropDatabase();

  // restore the data with --keepIndexVersion specified
  ret = toolTest.runTool.apply(toolTest, ['restore', '--keepIndexVersion']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored correctly
  assert.eq(10, testColl.count());

  // make sure the index version was not updated
  indexes = testColl.getIndexes();
  assert.eq(2, indexes.length);
  assert.eq(0, indexes[1].v);

  // success
  toolTest.stop();

}());
