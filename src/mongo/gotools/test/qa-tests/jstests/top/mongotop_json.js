// mongotop_json.js; ensure that running mongotop using the --json flag works as
// expected
var testName = 'mongotop_json';
(function() {
  jsTest.log('Testing mongotop --json option');
  load('jstests/top/util/mongotop_common.js');
  var assert = extendedAssert;

  var runTests = function(topology, passthrough) {
    jsTest.log('Using ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();

    // clear the output buffer
    clearRawMongoProgramOutput();

    // ensure tool runs without error with --rowcount = 1
    var ret = executeProgram(['mongotop', '--port', conn.port, '--json', '--rowcount', 1].concat(passthrough.args));
    assert.eq(ret.exitCode, 0, 'failed 1');
    assert.eq.soon('object', function() {
      return typeof JSON.parse(extractJSON(ret.getOutput()));
    }, 'invalid JSON 1');

    // ensure tool runs without error with --rowcount > 1
    var rowcount = 5;
    clearRawMongoProgramOutput();
    ret = executeProgram(['mongotop', '--port', conn.port, '--json', '--rowcount', rowcount].concat(passthrough.args));
    assert.eq(ret.exitCode, 0, 'failed 2');
    assert.eq.soon(rowcount, function() {
      return ret.getOutput().split('\n').length;
    }, "expected " + rowcount + " top results");
    ret.getOutput().split('\n').forEach(function(line) {
      assert(typeof JSON.parse(extractJSON(line)) === 'object', 'invalid JSON 2');
    });

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
  });
}());
