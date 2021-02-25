(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests that running mongorestore with --drop on a database with
  // nothing to drop does not error out, and completes the
  // restore successfully.

  jsTest.log('Testing restoration with --drop on a nonexistent db');

  var toolTest = getToolTest('drop_nonexistent_db');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'drop_nonexistent_db_dump';
  resetDbpath(dumpTarget);

  // the db we will use
  var testDB = toolTest.db.getSiblingDB('test');

  // insert a bunch of data
  var data = [];
  for (var i = 0; i < 500; i++) {
    data.push({_id: i});
  }
  testDB.coll.insertMany(data);
  // sanity check the insertion worked
  assert.eq(500, testDB.coll.count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the database we are using
  testDB.dropDatabase();
  // sanity check the drop worked
  assert.eq(0, testDB.coll.count());

  // restore the data with --drop
  ret = toolTest.runTool.apply(toolTest, ['restore', '--drop']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the data was restored
  assert.eq(500, testDB.coll.count());
  for (i = 0; i < 500; i++) {
    assert.eq(1, testDB.coll.count({_id: i}));
  }

  // success
  toolTest.stop();

}());
