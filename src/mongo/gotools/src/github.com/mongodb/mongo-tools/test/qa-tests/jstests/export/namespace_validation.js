(function() {

  // Tests running mongoexport with bad command line options.

  jsTest.log('Testing exporting valid or invalid namespaces');

  var toolTest = new ToolTest('system_collection');
  toolTest.startDB('foo');

  // run mongoexport with an dot in the db name
  ret = toolTest.runTool('export', '--db', 'test.bar', '--collection', 'foo');
  assert.neq(0, ret);

  // run mongoexport with an " in the db name
  ret = toolTest.runTool('export', '--db', 'test"bar', '--collection', 'foo');
  assert.neq(0, ret);

  // run mongoexport with a system collection
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'system.foobar');
  assert.eq(0, ret);

  // success
  toolTest.stop();

}());
