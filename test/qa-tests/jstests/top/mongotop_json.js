// mongotop_json.js; ensure that running mongotop using the --json flag works as
// expected
//
var testName = 'mongotop_json';
load('jstests/top/util/mongotop_common.js');

(function() {
  jsTest.log('Testing mongotop --json option');

  var runTests = function(topology, passthrough) {
    jsTest.log('Using ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();

    // clear the output buffer
    clearRawMongoProgramOutput();

    // ensure tool runs without error with --rowcount = 1
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--json', '--rowcount', 1].concat(passthrough.args)), 0, 'failed 1');
    assert(typeof JSON.parse(extractJSON(rawMongoProgramOutput())) === 'object', 'invalid JSON 1')

    // ensure tool runs without error with --rowcount > 1
    var rowcount = 5;
    clearRawMongoProgramOutput();
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--json', '--rowcount', rowcount].concat(passthrough.args)), 0, 'failed 2');
    var shellOutput = rawMongoProgramOutput();
    var outputLines = shellOutput.split('\n').filter(function(line) {
      return line.match(shellOutputRegex);
    });

    assert.eq(rowcount, outputLines.length, "expected " + rowcount + " top results but got " + outputLines.length);

    outputLines.forEach(function(line) {
      assert(typeof JSON.parse(extractJSON(line)) === 'object', 'invalid JSON 2')
    });

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
  });
})();