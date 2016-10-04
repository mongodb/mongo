(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  var toolTest = getToolTest('stat_broken_pipe');
  var baseArgs = getCommonToolArguments();
  baseArgs = baseArgs.concat('--port', toolTest.port);

  if (toolTest.useSSL) {
    baseArgs = baseArgs.concat([
      '--ssl',
      '--sslPEMKeyFile', 'jstests/libs/server.pem',
      '--sslCAFile', 'jstests/libs/ca.pem',
      '--sslAllowInvalidHostnames']);
  }
  var statArgs = ['mongostat', '--rowcount=5'].concat(baseArgs);
  var ddArgs = ['dd', 'count=1000000', 'bs=1024', 'of=/dev/null'];
  if (_isWindows()) {
    statArgs[0] += '.exe';
  }
  statArgs.unshift('set -o pipefail', '&&', 'PATH=.:$PATH');

  var ret = runProgram('bash', '-c', statArgs.concat('|', ddArgs).join(' '));
  assert.eq(0, ret, "bash execution should succeed");

  ddArgs = ['dd', 'count=100', 'bs=1', 'of=/dev/null'];
  ret = runProgram('bash', '-c', statArgs.concat('|', ddArgs).join(' '));
  assert.neq(0, ret, "bash execution should fail");
  assert.soon(function() {
    return rawMongoProgramOutput().search(/broken pipe|The pipe is being closed/);
  }, 'should print an error message');

  toolTest.stop();
}());
