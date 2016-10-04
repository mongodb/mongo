(function() {

  load("jstests/configs/standard_dump_targets.config.js");

  // Tests running mongorestore with bad command line options.

  jsTest.log('Testing running mongorestore with bad'+
            ' command line options');

  var toolTest = new ToolTest('incompatible_flags');
  toolTest.startDB('foo');

  // run restore with both --objcheck and --noobjcheck specified
  var ret = toolTest.runTool.apply(toolTest, ['restore',
      '--objcheck', '--noobjcheck']
    .concat(getRestoreTarget('restore/testdata/dump_empty')));
  assert.neq(0, ret);

  // run restore with --oplogLimit with a bad timestamp
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--oplogReplay', '--oplogLimit',
      'xxx']
    .concat(getRestoreTarget('restore/testdata/dump_with_oplog')));
  assert.neq(0, ret);

  // run restore with a negative --w value
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--w', '-1']
    .concat(getRestoreTarget('jstests/restore/testdata/dump_empty')));
  assert.neq(0, ret);

  // run restore with an invalid db name
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'billy.crystal']
    .concat(getRestoreTarget('jstests/restore/testdata/blankdb')));
  assert.neq(0, ret);

  // run restore with an invalid collection name
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--db', 'test',
      '--collection', '$money']
    .concat(getRestoreTarget('jstests/restore/testdata/blankcoll/blank.bson')));
  assert.neq(0, ret);

  // run restore with an invalid verbosity value
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '-v', 'torvalds']
    .concat(getRestoreTarget('restore/testdata/dump_empty')));
  assert.neq(0, ret);

  // success
  toolTest.stop();

}());
