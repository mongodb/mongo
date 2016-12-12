// mongofiles_search.js; ensures that the search command returns any and all
// files that match the regex supplied
var testName = 'mongofiles_search';
(function() {
  load('jstests/files/util/mongofiles_common.js');
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var conn;

  // Given a list of search strings and an expected result - 0 for present or 1 for
  // hasMatch takes in raw mongofiles search output and a matchItem; it returns 0
  // if it finds the match item in any line of the output and 1 otherwise. If the
  // exactString argument is not empty, hasMatch further checks that the line
  // matches the argument
  var hasMatch = function(output, matchItem, exactString) {
    var lines = output.split('\n');
    var shellOutputRegex = /^sh.*/;
    for (var i = 0; i < lines.length; i++) {
      if (lines[i].match(shellOutputRegex) && lines[i].match(matchItem)) {
        if (exactString && !lines[i].match(exactString)) {
          continue;
        }
        return 0;
      }
    }
    // matchItem wasn't found
    return 1;
  };

  // note - assertHasFiles checks that the output of running mongofiles search with
  // each of the search strings meets the expected result supplied. If exactString
  // is not empty, it further checks that the output also matches exactString
  var assertHasFiles = function(passthrough, searchStrings, expectedResult, exactString) {
    // perform a couple of search commands against the GridFS collection
    for (var i = 0; i < searchStrings.length; i++) {
      clearRawMongoProgramOutput();
      var queryString = searchStrings[i];
      assert.eq(runMongoProgram.apply(this, ['mongofiles',
          '--quiet',
          '--port', conn.port,
          'search', queryString]
        .concat(passthrough.args)),
        0, 'search command failed on ' + queryString + ' - part of ' + searchStrings);

      // eslint-disable-next-line no-loop-func
      assert.eq.soon(expectedResult, function() {
        return hasMatch(rawMongoProgramOutput(), queryString, exactString);
      }, 'search failed: expected "' + queryString + '" to be ' + (expectedResult ? 'found' : 'missing'));
    }
  };

  var runTests = function(topology, passthrough) {
    jsTest.log('Testing mongofiles search command');
    var t = topology.init(passthrough);
    conn = t.connection();

    jsTest.log('Putting files into GridFS with ' + passthrough.name + ' passthrough');

    for (var i = 0; i < filesToInsert.length; i++) {
      assert.eq(runMongoProgram.apply(this, ['mongofiles',
            '--port', conn.port,
            'put', filesToInsert[i]]
        .concat(passthrough.args)),
          0, 'put failed on ' + filesToInsert[i] + ' when it should have succeeded');
    }

    jsTest.log('Searching files in GridFS with ' + passthrough.name + ' passthrough');

    // these search strings should be matched
    var searchStrings = ['files', '.txt', 'ile', '.'];

    // add the verbatim file names put into GridFS
    for (i = 0; i < filesToInsert.length; i++) {
      searchStrings.push(filesToInsert[i]);
    }

    // all inserted files should be returned
    assertHasFiles(passthrough, searchStrings, 0);

    // these search strings should NOT be matched
    searchStrings = ['random', 'always', 'filer'];
    assertHasFiles(passthrough, searchStrings, 1);

    // test that only the requested file is returned
    for (i = 0; i < filesToInsert.length; i++) {
      var currentFile = filesToInsert[i];
      jsTest.log('Searching for file ' + currentFile + ' with ' + passthrough.name + ' passthrough');

      // ensure the requested file is returned
      assertHasFiles(passthrough, [currentFile], 0);

      // ensure no other files are returned
      assertHasFiles(passthrough,
        // eslint-disable-next-line no-loop-func
        filesToInsert.filter(function(file) {
          return file !== currentFile;
        }), 1, currentFile);
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
