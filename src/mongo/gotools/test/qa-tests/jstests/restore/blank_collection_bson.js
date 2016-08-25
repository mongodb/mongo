(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  // Tests using mongorestore to restore data from a blank collection
  // file, with both a missing and blank metadata file.

  jsTest.log('Testing restoration from a blank collection file');

  var toolTest = getToolTest('blank_collection_bson');
  var commonToolArgs = getCommonToolArguments();

  // run the restore with the blank collection file and no
  // metadata file. it should succeed, but insert nothing.
  var ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'test',
      '--collection', 'blank']
    .concat(getRestoreTarget('jstests/restore/testdata/blankcoll/blank.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);
  assert.eq(0, toolTest.db.getSiblingDB('test').blank.count());

  // run the restore with the blank collection file and a blank
  // metadata file. it should succeed, but insert nothing.
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'test',
      '--collection', 'blank']
    .concat(getRestoreTarget('jstests/restore/testdata/blankcoll/blank_metadata.bson'))
    .concat(commonToolArgs));
  assert.eq(0, ret);
  assert.eq(0, toolTest.db.getSiblingDB('test').blank.count());

  // success
  toolTest.stop();

}());
