(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('export_broken_pipe');
  var baseArgs = getCommonToolArguments();
  baseArgs = baseArgs.concat('--port', toolTest.port);

  if (toolTest.useSSL) {
    baseArgs = baseArgs.concat([
      '--ssl',
      '--sslPEMKeyFile', 'jstests/libs/server.pem',
      '--sslCAFile', 'jstests/libs/ca.pem',
      '--sslAllowInvalidHostnames']);
  }
  var exportArgs = ['mongoexport', '-d', toolTest.db.getName(), '-c', 'foo'].concat(baseArgs);
  var ddArgs = ['dd', 'count=1000000', 'bs=1024', 'of=/dev/null'];
  if (_isWindows()) {
    exportArgs[0] += '.exe';
  }
  exportArgs.unshift('set -o pipefail', '&&', 'PATH=.:$PATH');

  var testDb = toolTest.db;
  testDb.dropDatabase();
  for (var i = 0; i < 500; i++) {
    testDb.foo.insert({i: i});
  }
  assert.eq(500, testDb.foo.count(), 'foo should have our test documents');

  var ret = runProgram('bash', '-c', exportArgs.concat('|', ddArgs).join(' '));
  assert.eq(0, ret, "bash execution should succeed");
  assert.strContains.soon('exported 500 records', rawMongoProgramOutput, 'should print the success message');

  ddArgs = ['dd', 'count=100', 'bs=1', 'of=/dev/null'];
  ret = runProgram('bash', '-c', exportArgs.concat('|', ddArgs).join(' '));
  assert.neq(0, ret, "bash execution should fail");
  assert.soon(function() {
    return rawMongoProgramOutput().search(/broken pipe|The pipe is being closed/);
  }, 'should print an error message');

  testDb.dropDatabase();
  toolTest.stop();
}());
