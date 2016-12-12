// mongofiles_replace.js; ensure that after putting a file once multiple times,
// on using --replace, any and all occurences of the given file is replaced in
// the GridFS collection - all other files are left as is
var testName = 'mongofiles_replace';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles --replace option');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    jsTest.log('Running put on file with --replace with ' + passthrough.name + ' passthrough');

    // insert the same file a couple of times
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 1');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 2');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 3');

    // ensure that it is never overwritten
    db.fs.files.findOne({
      filename: filesToInsert[0]
    });

    assert.eq(db.fs.files.count(), 3, 'expected 3 files inserted but got ' + db.fs.files.count());

    // now run with --replace
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '--replace',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 4');

    assert.eq(db.fs.files.count(), 1, 'expected 1 file inserted but got ' + db.fs.files.count());

    // insert other files but ensure only 1 is replaced
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[1]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 5');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'put', filesToInsert[2]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 6');
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        '--replace',
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'put failed when it should have succeeded 7');

    assert.eq(db.fs.files.count(), 3, 'expected 3 files inserted but got ' + db.fs.files.count());

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
