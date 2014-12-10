// mongofiles1.js; tests the mongofiles list option by doing the following:
// 
// 1. Inserts the mongod/mongo binaries using mongofiles put
// 2. Checks that the actual md5 of the file matches what's stored in the database
// 3. Runs the mongofiles list command to view all files stored.
// 4. Ensures that all the files inserted and returned. 
// 5. Ensures that the returned list matches thae actual filesToInsert[0] and size of
//    files inserted.
//
var testName = 'mongofiles_list';
load('jstests/files/util/mongofiles_common.js');

(function() {
  jsTest.log('Testing mongofiles list command');

  var putFile = function(passthrough, conn, file) {
    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles', '--port', conn.port, 'put', file].concat(passthrough.args)), 0, 'put for ' + file + 'failed');
    var db = conn.getDB('test');
    var fileObj = db.fs.files.findOne({
      filename: file,
    });
    assert(fileObj, 'could not find put file ' + file);
    assert.eq(md5sumFile(file), fileObj.md5, file + ' md5 did not match - expected ' + md5sumFile(file) + ' got ' + fileObj.md5);
    return fileObj.length;
  };

  var runTests = function(topology, passthrough) {
    jsTest.log('Putting GridFS files with ' + passthrough.name + ' passthrough');
  
    var inputFileRegex = /^sh.*files.*/;
    var whitespaceSplitRegex = /,?\s+/;
    var fileSizes = [];

    var t = topology.init(passthrough);
    var conn = t.connection();

    filesToInsert.forEach(function(file) {
      var fileSize = putFile(passthrough, conn, file);
      fileSizes.push(fileSize);
    });

    jsTest.log('Running mongofiles list');

    // clear the output buffer
    clearRawMongoProgramOutput();

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ['mongofiles', '--port', conn.port, '--quiet', 'list'].concat(passthrough.args)), 0, 'list command failed but was expected to succeed');

    var files = rawMongoProgramOutput().split('\n');
    var index = 0;

    jsTest.log('Verifying list output');

    // ensure that the returned files and their sizes are as expected
    files.forEach(function(currentFile) {
      if (currentFile.match(inputFileRegex)) {
        // should print mongod and then mongo
        var fileEntry = currentFile.split(whitespaceSplitRegex);

        // the list command should have 2 entries - the file name and its size
        // we check for 3 files because of the sh. prefix in our js test framework
        assert.eq(fileEntry.length, 3, 'unexpected list output on ' + currentFile + ' - expected 3 but got ' + fileEntry.length);

        // ensure the expected file name is what is printed
        assert.eq(fileEntry[1], filesToInsert[index], 'expected file ' + filesToInsert[1] + ' got ' + fileEntry[1]);

        // ensure the expected file size is what is printed
        assert.eq(fileEntry[2], fileSizes[index], 'expected size ' + fileSizes[2] + ' got ' + fileEntry[2]);
        index++;
      }
    });
    assert.neq(index, 0, 'list did not return any expected files')
    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
})();