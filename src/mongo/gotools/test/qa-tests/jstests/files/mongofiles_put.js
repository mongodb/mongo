// mongofiles_put.js; ensure that put works with very large files.
// NOTE: this test uses mongodump to create a large file
var testName = 'mongofiles_put';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles put command');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB('test');

    // create a large collection and dump it
    jsTest.log('Creating large collection with ' + passthrough.name + ' passthrough');

    var insertString = new Array(100).join("mongoDB");
    var inserted = 0;
    var num = 0;
    var dbName = 'test';
    var collection = 'foo';
    var bulk = db[collection].initializeUnorderedBulkOp();

    while (inserted < (40 * 1024 * 1024)) {
      bulk.insert({
        _id: num++,
        str: insertString
      });
      inserted += insertString.length;
    }

    assert.writeOK(bulk.execute({w: "majority"}));

    // dumping large collection to single large file
    jsTest.log('Dumping collection to filesystem with ' + passthrough.name + ' passthrough');

    var dumpDir = './dumpDir';

    assert.eq(runMongoProgram.apply(this, ['mongodump',
          '-d', dbName,
          '--port', conn.port,
          '-c', collection,
          '--out', dumpDir]
      .concat(passthrough.args)),
        0, 'dump failed when it should have succeeded');

    jsTest.log('Putting directory');

    // putting a directory should fail
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          'put', dumpDir]
      .concat(passthrough.args)),
        0, 'put succeeded when it should have failed');

    jsTest.log('Putting file with ' + passthrough.name + ' passthrough');

    var putFile = dumpDir + '/' + dbName + '/' + collection + '.bson';

    // ensure putting of the large file succeeds
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', putFile,
          'put', testName]
      .concat(passthrough.args)),
        0, 'put failed when it should have succeeded');

    // verify file metadata
    var fileObj = db.fs.files.findOne({
      filename: testName
    });
    assert(fileObj, testName + ' was not found');

    var numDbChunks = db.fs.chunks.count();

    // the number of chunks should be equal to math.ceil[fileSize (KB) / 255 KB]
    // filesize for the dump should be s bytes
    var expectedNumChunks = Math.ceil(fileObj.length / (1024 * 255));

    assert.eq(expectedNumChunks, numDbChunks, 'expected ' + expectedNumChunks + ' chunks; got ' + numDbChunks);

    // now attempt to get the large file
    jsTest.log('Getting file with ' + passthrough.name + ' passthrough');

    // ensure tool runs without error
    var getFile = testName + (Math.random() + 1).toString(36).substring(7);
    assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--port', conn.port,
          '--local', getFile,
          'get', testName]
      .concat(passthrough.args)),
        0, 'get failed');

    // ensure the retrieved file is exactly the same as that inserted
    var actual = md5sumFile(putFile);
    var expected = md5sumFile(getFile);

    assert.eq(actual, expected, 'mismatched md5 sum - expected ' + expected + ' got ' + actual);

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
