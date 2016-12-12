(function() {

  // Tests running mongoexport with no data in the target collection.

  jsTest.log('Testing exporting no data');

  var toolTest = new ToolTest('no_data');
  toolTest.startDB('foo');

  // run mongoexport with no data, make sure it doesn't error out
  var ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data');
  assert.eq(0, ret);

  // but it should fail if --assertExists specified
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data', '--assertExists');
  assert.neq(0, ret);

  // success
  toolTest.stop();

}());
