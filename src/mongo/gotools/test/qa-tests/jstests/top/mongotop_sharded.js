// mongotop_sharded.js; ensure that running mongotop against a sharded cluster
// fails with a useful error message
var testName = 'mongotop_sharded';
(function() {
  jsTest.log('Testing mongotop against sharded cluster');
  load('jstests/top/util/mongotop_common.js');
  var assert = extendedAssert;

  var expectedError = 'cannot run mongotop against a mongos';
  var verifyOutput = function(getOutput) {
    assert.strContains.soon(expectedError, getOutput, 'error message must appear at least once');
    var shellOutput = getOutput();
    jsTest.log('shell output: ' + shellOutput);
    shellOutput.split('\n').forEach(function(line) {
      // check the displayed error message
      assert.neq(line.match(expectedError), null, 'unexpeced error message');
    });
  };

  var runTests = function(topology, passthrough) {
    jsTest.log('Using ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();

    // getting the version should work without error
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--version'].concat(passthrough.args)), 0, 'failed 1');

    // getting the help text should work without error
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--help'].concat(passthrough.args)), 0, 'failed 2');

    // anything that runs against the mongos server should fail
    var result = executeProgram(['mongotop', '--port', conn.port].concat(passthrough.args));
    assert.neq(result.exitCode, 0, 'expected failure against a mongos');
    verifyOutput(result.getOutput);

    result = executeProgram(['mongotop', '--port', conn.port, '2'].concat(passthrough.args));
    assert.neq(result.exitCode, 0, 'expected failure against a mongos');
    verifyOutput(result.getOutput);

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(shardedClusterTopology, passthrough);
  });
}());
