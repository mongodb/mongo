// mongofiles_host.js; ensure that running mongofiles using valid and invalid
// host names or IP addresses succeeds/fails as expected
var testName = 'mongofiles_host';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --host option');

  var runTests = function(topology, passthrough) {
    jsTest.log('Putting file with valid host name with ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '--host', 'localhost',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 1 failed');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '--host', '127.0.0.1',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 2 failed');

    // ensure the file was inserted
    assert.eq(2, db.getCollection('fs.files').count(), 'unexpected fs.files count 1');

    jsTest.log('Putting file with invalid host name with ' + passthrough.name + ' passthrough');

    // ensure tool exits with a non-zero exit code when supplied invalid hosts
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--host', 'does-not-exist',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'expected mongofiles to fail but it succeeded 1');
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--host', '555.555.555.555',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'expected mongofiles to fail but it succeeded 2');

    // ensure the file was not inserted
    assert.eq(2, db.getCollection('fs.files').count(), 'unexpected fs.files count 2');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
