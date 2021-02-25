(function() {
  var bsondumpArgs = ['bsondump', '--type=json', 'jstests/bson/testdata/all_types.bson'];
  var ddArgs = ['dd', 'count=1000000', 'bs=1024', 'of=/dev/null'];
  if (_isWindows()) {
    bsondumpArgs[0] += '.exe';
  }
  bsondumpArgs.unshift('set -o pipefail', '&&', 'PATH=.:$PATH');

  var ret = runProgram('bash', '-c', bsondumpArgs.concat('|', ddArgs).join(' '));
  assert.eq(0, ret, "bash execution should succeed");

  ddArgs = ['dd', 'count=0', 'bs=1', 'of=/dev/null'];
  ret = runProgram('bash', '-c', bsondumpArgs.concat('|', ddArgs).join(' '));
  assert.neq(0, ret, "bash execution should fail");
  assert.soon(function() {
    return rawMongoProgramOutput().search(/broken pipe|The pipe is being closed/);
  }, 'should print an error message');
}());
