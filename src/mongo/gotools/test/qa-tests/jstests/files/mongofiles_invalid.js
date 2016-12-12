// mongofiles_invalid.js; runs mongofiles with an invalid command and
// option - ensures it fails in all cases
var testName = 'mongofiles_invalid';
load('jstests/files/util/mongofiles_common.js');
(function() {
  jsTest.log('Testing mongofiles with invalid commands and options');

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();

    jsTest.log('Running with file with invalid options onw passthrough ' + passthrough.name);

    // run with invalid option
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
        '--invalid', conn.port,
        'put', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'invalid-option: mongofiles succeeded when it should have failed');

    // run with invalid command
    assert.neq(runMongoProgram.apply(this, ['mongofiles',
        '--port', conn.port,
        'invalid', filesToInsert[0]]
      .concat(passthrough.args)),
      0, 'invalid-command: mongofiles succeeded when it should have failed');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });
}());
