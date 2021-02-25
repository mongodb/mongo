// This test runs bsondump on a .bson file containing non-deprecated BSON types
// and makes sure their debug type values exist in the output.
(function() {
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var x = _runMongoProgram("bsondump", "--type=debug", "jstests/bson/testdata/all_types.bson");
  assert.eq(x, 0, "bsondump should exit successfully with 0");

  var results;
  assert.eq.soon(22, function() {
    results = rawMongoProgramOutput();
    return (results.match(/--- new object ---/g) || []).length;
  }, "should see all documents from the test data");

  assert.strContains("type:    1", results, "bson type '1' should be present in the debug output");
  assert.strContains("type:    2", results, "bson type '2' should be present in the debug output");
  assert.strContains("type:    3", results, "bson type '3' should be present in the debug output");
  assert.strContains("type:    4", results, "bson type '4' should be present in the debug output");
  assert.strContains("type:    5", results, "bson type '5' should be present in the debug output");
  assert.strContains("type:    6", results, "bson type '6' should be present in the debug output");
  assert.strContains("type:    7", results, "bson type '7' should be present in the debug output");
  assert.strContains("type:    8", results, "bson type '8' should be present in the debug output");
  assert.strContains("type:    9", results, "bson type '9' should be present in the debug output");
  assert.strContains("type:   10", results, "bson type '10' should be present in the debug output");
  assert.strContains("type:   11", results, "bson type '11' should be present in the debug output");
  assert.strContains("type:   12", results, "bson type '12' should be present in the debug output");
  assert.strContains("type:   13", results, "bson type '13' should be present in the debug output");
  assert.strContains("type:   17", results, "bson type '17' should be present in the debug output");
  assert.strContains("type:   18", results, "bson type '18' should be present in the debug output");
  assert.strContains("type:   -1", results, "bson type '-1' should be present in the debug output");
  assert.strContains("type:  127", results, "bson type '127' should be present in the debug output");
}());
