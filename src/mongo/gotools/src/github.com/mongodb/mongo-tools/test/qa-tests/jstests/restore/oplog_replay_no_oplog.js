(function() {

  load("jstests/configs/standard_dump_targets.config.js");
  // Tests using mongorestore with --oplogReplay when no oplog.bson file is present.

  jsTest.log('Testing restoration with --oplogReplay and no oplog.bson file');

  var toolTest = new ToolTest('oplog_replay_no_oplog');
  toolTest.startDB('foo');

  // run the restore, with a dump directory that has no oplog.bson file
  var ret = toolTest.runTool.apply(toolTest, ['restore', '--oplogReplay']
    .concat(getRestoreTarget('restore/testdata/dump_empty')));
  assert.neq(0, ret);

  // success
  toolTest.stop();

}());
