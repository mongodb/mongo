(function() {

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  if (dump_targets === "archive") {
    print('skipping test incompatable with archiving');
    return assert(true);
  }

  // Tests using mongorestore to restore data from a blank db directory.

  jsTest.log('Testing restoration from a blank db directory');

  var toolTest = getToolTest('blank_db');
  var commonToolArgs = getCommonToolArguments();

  // run the restore with the blank db directory. it should succeed, but
  // insert nothing.
  var ret = toolTest.runTool.apply(toolTest, ['restore', '--db', 'test']
    .concat(getRestoreTarget('jstests/restore/testdata/blankdb'))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // success
  toolTest.stop();

}());
