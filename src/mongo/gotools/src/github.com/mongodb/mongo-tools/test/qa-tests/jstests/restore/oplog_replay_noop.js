(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  // Tests using mongorestore with --oplogReplay and noops in the oplog.bson,
  // making sure the noops are ignored.

  jsTest.log('Testing restoration with --oplogReplay and noops');

  var toolTest = getToolTest('oplog_replay_noop');
  var commonToolArgs = getCommonToolArguments();

  // the db and collection we'll be using
  var testDB = toolTest.db.getSiblingDB('test');
  var testColl = testDB.data;

  // restore the data, with --oplogReplay
  var ret = toolTest.runTool.apply(toolTest, ['restore', '--oplogReplay']
    .concat(getRestoreTarget('jstests/restore/testdata/dump_with_noop_in_oplog'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // make sure the document appearing in the oplog, which shows up
  // after the noops, was added successfully
  assert.eq(1, testColl.count());
  assert.eq(1, testColl.count({a: 1}));

  toolTest.stop();

}());
