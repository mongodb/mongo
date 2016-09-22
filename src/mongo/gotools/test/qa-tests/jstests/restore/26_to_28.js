// This test requires mongo 2.6.x, and mongo 3.0.0 releases
// @tags: [requires_mongo_26, requires_mongo_30]
(function() {

  load("jstests/configs/standard_dump_targets.config.js");

  // skip tests requiring wiredTiger storage engine on pre 3.0 mongod
  if (TestData && TestData.storageEngine === 'wiredTiger') {
    return;
  }

  // Tests using mongorestore to restore a dump from a 2.6 mongod to a 3.0 mongod.
  jsTest.log('Testing running mongorestore restoring data from a 2.6 mongod to'+
      ' a 3.0 mongod');

  var toolTest = new ToolTest('26_to_28', {binVersion: '2.6'});
  toolTest.startDB('foo');

  // where we'll put the dump
  var dumpTarget = '26_to_28_dump';
  resetDbpath(dumpTarget);

  // the db and collection we'll be using
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.coll;

  // insert some documents
  var data = [];
  for (var i = 0; i < 50; i++) {
    data.push({_id: i});
  }
  testColl.insertMany(data);
  // sanity check the insert worked
  assert.eq(50, testColl.count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump'].concat(getDumpTarget(dumpTarget)));
  assert.eq(0, ret);

  // drop the database
  testDB.dropDatabase();

  // restart the mongod as a 3.0
  stopMongod(toolTest.port);
  toolTest.m = null;
  toolTest.db = null;
  delete toolTest.options.binVersion;
  toolTest.startDB('foo');

  // refresh the db and coll reference
  testDB = toolTest.db.getSiblingDB('test');
  testColl = testDB.coll;

  // restore the data
  ret = toolTest.runTool.apply(toolTest, ['restore'].concat(getRestoreTarget(dumpTarget)));
  assert.eq(0, ret);

  // make sure the data was restored
  assert.eq(50, testColl.count());
  for (i = 0; i < 50; i++) {
    assert.eq(1, testColl.count({_id: i}));
  }

  // success
  toolTest.stop();

}());
