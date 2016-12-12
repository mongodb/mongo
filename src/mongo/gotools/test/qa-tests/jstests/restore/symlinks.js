(function() {

  // Tests using mongorestore on a dump directory containing symlinks

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  jsTest.log('Testing restoration from a dump containing symlinks');

  var toolTest = getToolTest('symlinks');

  // this test uses the testdata/dump_with_soft_link. within that directory,
  // the dbTwo directory is a soft link to testdata/soft_linked_db and the
  // dbOne/data.bson file is a soft link to testdata/soft_linked_collection.bson.
  // the file not_a_dir is a softlink to a bson file, and is there to make
  // sure that softlinked regular files are not treated as directories.

  // the two dbs we'll be using
  var dbOne = toolTest.db.getSiblingDB('dbOne');
  var dbTwo = toolTest.db.getSiblingDB('dbTwo');
  var notADir = toolTest.db.getSiblingDB('not_a_dir');

  // restore the data
  var ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget('jstests/restore/testdata/dump_with_soft_links')));
  assert.eq(0, ret);

  // make sure the data was restored properly
  assert.eq(10, dbOne.data.count());
  assert.eq(10, dbTwo.data.count());
  assert.eq(0, notADir.data.count());
  for (var i = 0; i < 10; i++) {
    assert.eq(1, dbOne.data.count({_id: i+'_dbOne'}));
    assert.eq(1, dbTwo.data.count({_id: i+'_dbTwo'}));
  }

  // success
  toolTest.stop();

}());
