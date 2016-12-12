// mongofiles_version.js; ensure that getting the version works without error
var testName = 'mongofiles_version';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --version option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();

    jsTest.log('Testing --version with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '--version']
      .concat(passthrough.args)),
      0, '--version failed');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
