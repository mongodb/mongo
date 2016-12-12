(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  // Tests using mongorestore to restore a mix of existing and
  // non-existing documents to a collection, so we can make sure
  // all new documents are actually added.

  jsTest.log('Testing restoration of a dump on top of existing documents');

  var toolTest = getToolTest('dupe_restore');
  var commonToolArgs = getCommonToolArguments();

  // where we'll put the dump
  var dumpTarget = 'dupe_restore_dump';
  resetDbpath(dumpTarget);

  // we'll insert data into three collections spread across two dbs
  var dbOne = toolTest.db.getSiblingDB('dbOne');
  var testColl = dbOne.duplicates;

  // insert a bunch of data
  var data = [];
  for (var i = 0; i < 50; i++) {
    data.push({_id: i});
  }
  testColl.insertMany(data);
  // sanity check the insertion worked
  assert.eq(50, testColl.count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // remove a few random documents
  var removeDocs = function() {
    testColl.remove({_id: 0});
    testColl.remove({_id: 5});
    testColl.remove({_id: 6});
    testColl.remove({_id: 9});
    testColl.remove({_id: 12});
    testColl.remove({_id: 27});
    testColl.remove({_id: 40});
    testColl.remove({_id: 46});
    testColl.remove({_id: 47});
    testColl.remove({_id: 49});
    assert.eq(40, testColl.count());
  };
  removeDocs();

  // restore the db with default settings
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the restore worked, and all of the removed keys were restored
  assert.eq(50, testColl.count(), "some documents were not restored with default settings");

  // now check an array of batch sizes
  for (i = 1; i < 100; i++) {
    removeDocs();
    ret = toolTest.runTool.apply(toolTest, ['restore', "--batchSize", String(i)]
      .concat(getRestoreTarget(dumpTarget))
      .concat(commonToolArgs));
    assert.eq(0, ret);
    assert.eq(50, testColl.count(), "some documents were not restored for batchSize="+i);
  }

  toolTest.stop();
}());
