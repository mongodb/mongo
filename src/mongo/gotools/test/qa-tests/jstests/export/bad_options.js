(function() {

  // Tests running mongoexport with bad command line options.

  jsTest.log('Testing running mongoexport with bad command line options');

  var toolTest = new ToolTest('bad_options');
  toolTest.startDB('foo');

  // run mongoexport with a missing --collection argument
  var ret = toolTest.runTool('export', '--db', 'test');
  assert.neq(0, ret);

  // run mongoexport with bad json as the --query
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--query', '{ hello }');
  assert.neq(0, ret);

  // run mongoexport with a bad argument to --skip
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--sort', '{a: 1}', '--skip', 'jamesearljones');
  assert.neq(0, ret);

  // run mongoexport with a bad argument to --sort
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--sort', '{ hello }');
  assert.neq(0, ret);

  // run mongoexport with a bad argument to --limit
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--sort', '{a: 1}', '--limit', 'jamesearljones');
  assert.neq(0, ret);

  // run mongoexport with --query and --queryFile
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--query', '{a:1}', '--queryFile', 'jstests/export/testdata/query.json');
  assert.neq(0, ret);

  // run mongoexport with a --queryFile that doesn't exist
  ret = toolTest.runTool('export', '--db', 'test', '--collection', 'data',
      '--queryFile', 'jstests/nope');
  assert.neq(0, ret);

  // success
  toolTest.stop();

}());
