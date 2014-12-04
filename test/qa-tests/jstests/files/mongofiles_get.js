// mongofiles_get.js; ensure that get command works as expected
//

var testName = "mongofiles_get";
load("jstests/files/mongofiles_common.js");

(function() {
  jsTest.log("Testing mongofiles get command");

  var runTests = function(topology, passthrough) {
    var t = topology.init(passthrough);
    var conn = t.connection();
    var db = conn.getDB("test");
    var getFile = testName + (Math.random() + 1).toString(36).substring(7);

    jsTest.log("Putting file with '" + passthrough.name + "' passthrough");

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ["mongofiles", "--port", conn.port, "put", filesToInsert[0]].concat(passthrough.args)), 0, "put 1 failed");

    // ensure the file was inserted
    assert.eq(1, db.fs.files.count(), "unexpected fs.files count 1");

    jsTest.log("Getting file with '" + passthrough.name + "' passthrough");

    // ensure tool runs without error
    assert.eq(runMongoProgram.apply(this, ["mongofiles", "--port", conn.port, "--local", getFile, "get", filesToInsert[0]].concat(passthrough.args)), 0, "put 2 failed");

    // ensure the retrieved file is exactly the same as that inserted
    var actual = md5sumFile(filesToInsert[0]);
    var expected = md5sumFile(getFile);

    assert.eq(actual, expected, "mismatched md5 sum - expected " + expected + " got " + actual);

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
    runTests(shardedClusterTopology, passthrough);
  });

})();