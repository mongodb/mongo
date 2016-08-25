(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('norestore_profile');
  var commonToolArgs = getCommonToolArguments();

  var dbOne = toolTest.db.getSiblingDB('dbOne');
  // turn on the profiler
  dbOne.setProfilingLevel(2);

  // create some test data
  for (var i=0; i<=100; i++) {
    dbOne.test.insert({_id: i, x: i*i});
  }
  // run some queries to end up in the profile collection
  dbOne.test.find({_id: 3});
  dbOne.test.find({_id: 30});
  dbOne.test.find({_id: 50});

  assert.gt(dbOne.system.profile.count(), 0, "profiler still empty after running test setup");

  // dump it
  var dumpTarget = 'norestore_profile';
  resetDbpath(dumpTarget);
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // turn off profiling and remove the profiler collection
  dbOne.setProfilingLevel(0);
  dbOne.system.profile.drop();
  assert.eq(dbOne.system.profile.count(), 0);

  // drop the database so it's empty
  dbOne.dropDatabase();

  // restore it, this should restore everything *except* the profile collection
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, "restore to empty DB should have returned successfully");

  // check that the data actually got restored
  assert.gt(dbOne.test.count(), 100);

  // but the profile collection should still be empty
  assert.eq(dbOne.system.profile.count(), 0);

  // success
  toolTest.stop();

}());
