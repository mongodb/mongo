// mongofiles_port.js; ensure that supplying valid/invalid port addresses
// succeeds/fails as expected
var testName = 'mongofiles_port';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --port option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    jsTest.log('Putting file with valid port with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 1 failed');

    // ensure the file was inserted
    assert.eq(1, db.fs.files.count(), 'unexpected fs.files count 1');

    jsTest.log('Putting file with invalid port with ' + passthrough.name + ' passthrough');

    // ensure tool exits with a non-zero exit code when supplied invalid ports
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', '12345',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'expected mongofiles to fail but it succeeded 1');
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', 'random',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'expected mongofiles to fail but it succeeded 2');

    // ensure the file was not inserted
    var count = db.fs.files.count();
    assert.eq(1, count, 'unexpected fs.files count - expected 2 but got ' + count);

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
