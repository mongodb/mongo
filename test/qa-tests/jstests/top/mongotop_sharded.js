// mongotop_sharded.js; ensure that running mongotop against a sharded cluster
// fails with a useful error message
//
var testName = 'mongotop_sharded';
var expectedError = 'cannot run mongotop against a mongos';
load('jstests/top/util/mongotop_common.js');

(function() {
  jsTest.log('Testing mongotop against sharded cluster');

  var verifyOutput = function(shellOutput) {
    jsTest.log('shell output: ' + shellOutput);
    shellOutput.split('\n').forEach(function(line) {
      // check the displayed error message
      if (line.match(shellOutputRegex)) {
        assert.neq(line.match(expectedError), null, 'unexpeced error message');
      }
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
    clearRawMongoProgramOutput();
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port].concat(passthrough.args)), 0, 'succeeded 1');
    verifyOutput(rawMongoProgramOutput());

    clearRawMongoProgramOutput();
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '2'].concat(passthrough.args)), 0, 'succeeded 2');
    verifyOutput(rawMongoProgramOutput());

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(shardedClusterTopology, passthrough);
  });
})();