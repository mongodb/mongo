// mongofiles_db.js; ensure that running mongofiles using the db flag works as
// expected
var testName = 'mognofiles_db';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --host option');

  var runTests = function(topology, passthrough) {
    jsTest.log('Putting file with valid host name with ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('otherdb');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--db', 'otherdb',
        '--port', conn.port,
        '--host', 'localhost',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 1 failed');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--db', 'otherdb',
        '--port', conn.port,
        '--host', 'localhost',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put 2 failed');

    // ensure the files were inserted into the right db
    assert.eq(2, db.getCollection('fs.files').count(), 'unexpected fs.files count 1');

    // test short form
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '-d', 'otherdb',
          '--port', conn.port,
          '--host', 'localhost',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'put 3 failed');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '-d', 'otherdb',
          '--port', conn.port,
          '--host', 'localhost',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'put 4 failed');

    // ensure the file was inserted into the right db
    assert.eq(4, db.getCollection('fs.files').count(), 'unexpected fs.files count 2s');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
