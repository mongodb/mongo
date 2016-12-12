// mongofiles_local.js; ensure that when --local is passed:
// a. for puts, the supplied argument is read and stored using the gridfs filename
// b. for gets, the supplied argument is used to store the retrieved file
// c. for puts, if the supplied argument is the empty string, an error should occur
// d. for gets, if the supplied argument is the empty string, the file name is used
var testName = 'mongofiles_local';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --local option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    // generate a random GridFS name for the file
    var putFSName = testName + (Math.random() + 1).toString(36).substring(7);
    var getFSName = testName + (Math.random() + 1).toString(36).substring(7);

    jsTest.log('Running put on file with --local');

    // ensure tool runs without error with a non-empty --local argument
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '-l', filesToInsert[0],
        'put', putFSName]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 1');

    // ensure the file exists
    assert(db.fs.files.findOne({
      filename: putFSName
    }), 'did not find expected GridFS file - ' + putFSName);

    // ensure tool returns an error if the --local argument does not exist
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', filesToInsert[0] + '?',
          'put', putFSName]
      .concat(passthrough.args)),
        0, 'put succeeded when it should have failed 2');

    // if the argument is empty, use the putFSName - which should cause an error since it doesn't exist
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', '',
          'put', putFSName]
      .concat(passthrough.args)),
        0, 'put succeeded when it should have failed 3');

    // if the argument is empty, and the GridFS file exists, it should run
    // without error  on linux and fails on windows
    var comparison = 'eq';
    if (_isWindows()) {
      comparison = 'neq';
    }
    assert[comparison](runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', '',
          'put', filesToInsert[0]]
      .concat(passthrough.args)),
        0, 'put failed when it should have succeeded 2');

    jsTest.log('Running get on file with --local');

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', getFSName,
          'get', putFSName]
      .concat(passthrough.args)),
        0, 'get failed when it should have succeeded 1');

    // ensure the right file name was written
    assert.eq(md5sumFile(filesToInsert[0]), md5sumFile(getFSName), 'files do not match!');

    // ensure tool uses the GridFS name if the --local argument is empty on linux
    // and fails on windows
    comparison = 'eq';
    if (_isWindows()) {
      comparison = 'neq';
    }
    assert[comparison](runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', '',
          'get', putFSName]
      .concat(passthrough.args)),
        0, 'get failed unexpectedly');

    if (!_isWindows()) {
      assert.eq(md5sumFile(filesToInsert[0]), md5sumFile(putFSName), 'md5sums do not match - expected ' + md5sumFile(filesToInsert[0]) + ' got ' + md5sumFile(putFSName));
    }
    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
