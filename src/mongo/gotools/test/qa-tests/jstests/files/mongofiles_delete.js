// mongofiles_delete.js; ensure that delete command works as expected
var testName = 'mongofiles_delete';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles delete command');

  var runTests = function(topology, passthrough) {
    jsTest.log('Putting file with ' + passthrough.name + ' passthrough');

    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    // ensure tool runs without error
    for (var i = 0; i < 10; i++) {
      assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          'put', filesToInsert[0]]
        .concat(passthrough.args)),
        0, 'put failed');
    }

    // ensure all the files were written
    assert.eq(10, db.fs.files.count(), 'unexpected fs.files count');

    jsTest.log('Deleting file');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          'delete', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'delete failed');

    // ensure all the files were deleted
    assert.eq(0, db.fs.files.count(), 'unexpected fs.files count');
    assert.eq(0, db.fs.chunks.count(), 'unexpected fs.chunks count');
    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
