// mongofiles_prefix.js; ensure that passing --prefix works as expected - the
// provided prefix is used as the collection name prefix
var testName = 'mongofiles_prefix';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --prefix option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    jsTest.log('Putting file without --prefix with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 1 failed');

    // ensure the default collection name prefix was used
    assert.eq(1, db.fs.files.count(), 'unexpected fs.files count');
    assert.eq(0, db[testName + '.files'].count(), 'unexpected ' + testName + '.files count');

    jsTest.log('Putting file with --prefix with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--prefix', testName,
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'put 2 failed');

    // ensure the supplied collection name prefix was used
    assert.eq(1, db.fs.files.count(), 'unexpected fs.files count');
    assert.eq(1, db[testName + '.files'].count(), 'unexpected ' + testName + '.files count');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
