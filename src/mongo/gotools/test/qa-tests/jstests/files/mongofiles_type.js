// mongofiles_type.js; ensure that the given content type is stored when passed
// as the --type argument. If no argument is passed, it should be omitted in the
// database.
var testName = 'mongofiles_type';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --type option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');
    var contentType = 'txt';

    jsTest.log('Running put on file with --type with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error with a non-empty --type argument
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '-t', contentType,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 1');

    var fileObj = db.fs.files.findOne({
      filename: filesToInsert[0]
    });

    assert(fileObj, 'did not find expected GridFS file - ' + filesToInsert[0]);

    assert.eq(fileObj.contentType, contentType, 'unexpected content type - found ' + fileObj.contentType + ' but expected ' + contentType);

    // ensure tool runs without error with empty --type argument on linux
    // and fails on windows
    var comparison = 'eq';
    if (_isWindows()) {
      comparison = 'neq';
    }
    assert[comparison](runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--type', '',
          'put', filesToInsert[1]]
      .concat(passthrough.args)),
        0, 'put failed unexpectedly');

    if (!_isWindows()) {
      fileObj = db.fs.files.findOne({
        filename: filesToInsert[1]
      });
      assert.neq(fileObj, null, 'did not find expected GridFS file - ' + filesToInsert[1]);
      assert.eq(fileObj.contentType, undefined, 'unexpected content type - found ' + fileObj.contentType + ' but expected undefined');
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
