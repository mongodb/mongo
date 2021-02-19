(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  // Tests that using mongorestore on a collection with extended json types
  // in the metadata (both indexes and options) is handled gracefully.

  jsTest.log('Testing that restoration of extended JSON collection options works.');

  var toolTest = getToolTest('extended_json_metadata_restore');
  var commonToolArgs = getCommonToolArguments();
  var testDB = toolTest.db.getSiblingDB('test');
  assert.eq(testDB.changelog.exists(), null, "collection already exists in db");

  // run a restore against the mongos
  var ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget('jstests/restore/testdata/dump_extended_json_options'))
    .concat(commonToolArgs));
  assert.eq(0, ret, "the restore does not crash");

  var collectionOptionsFromDB = testDB.changelog.exists();
  printjson(collectionOptionsFromDB);
  assert.eq(collectionOptionsFromDB.options.capped, true, "capped option should be restored");
  // Mongodb might fudge the collection max values for different storage engines,
  // so we need some wiggle room.
  var delta = 1000;
  var size = 10 * 1000 * 1000;
  assert.lte(collectionOptionsFromDB.options.size, size+delta, "size should be ~10000000");
  assert.gte(collectionOptionsFromDB.options.size, size-delta, "size should be ~10000000");

  var indexes = testDB.changelog.getIndexes();
  printjson(indexes);
  assert.eq(indexes[0].key._id, 1, "index is read properly");

}());
