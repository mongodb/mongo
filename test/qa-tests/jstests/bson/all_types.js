// This test runs bsondump on a .bson file containing non-deprecated BSON types
// and makes sure their debug type values exist in the output.

(function(){
  var x = _runMongoProgram( "bsondump", "--type=debug", "jstests/bson/testdata/all_types.bson");
  assert.eq(x, 0, "bsondump should exit successfully with 0");

  sleep(100);

  var results = rawMongoProgramOutput();
  assert.gt(results.search("type:    1"), 0, "bson type '1' should be present in the debug output");
  assert.gt(results.search("type:    2"), 0, "bson type '2' should be present in the debug output");
  assert.gt(results.search("type:    3"), 0, "bson type '3' should be present in the debug output");
  assert.gt(results.search("type:    4"), 0, "bson type '4' should be present in the debug output");
  assert.gt(results.search("type:    5"), 0, "bson type '5' should be present in the debug output");
  assert.gt(results.search("type:    6"), 0, "bson type '6' should be present in the debug output");
  assert.gt(results.search("type:    7"), 0, "bson type '7' should be present in the debug output");
  assert.gt(results.search("type:    8"), 0, "bson type '8' should be present in the debug output");
  assert.gt(results.search("type:    9"), 0, "bson type '9' should be present in the debug output");
  assert.gt(results.search("type:   10"), 0, "bson type '10' should be present in the debug output");
  assert.gt(results.search("type:   11"), 0, "bson type '11' should be present in the debug output");
  assert.gt(results.search("type:   12"), 0, "bson type '12' should be present in the debug output")
  assert.gt(results.search("type:   13"), 0, "bson type '13' should be present in the debug output");
  assert.gt(results.search("type:   17"), 0, "bson type '17' should be present in the debug output");
  assert.gt(results.search("type:   18"), 0, "bson type '18' should be present in the debug output");
  assert.gt(results.search("type:   -1"), 0, "bson type '-1' should be present in the debug output");
  assert.gt(results.search("type:  127"), 0, "bson type '127' should be present in the debug output");
})();
