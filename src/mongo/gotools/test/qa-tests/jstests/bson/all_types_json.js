// This test runs bsondump on a .bson file containing non-deprecated BSON types
// and makes sure their JSON type representations exist in the output.
(function() {
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var x = _runMongoProgram("bsondump", "--type=json", "jstests/bson/testdata/all_types.bson");
  assert.eq(x, 0, "bsondump should exit successfully with 0");

  assert.strContains.soon("20 objects found", rawMongoProgramOutput,
      "should print out all top-level documents from the test data");

  var results = rawMongoProgramOutput();
  assert.strContains("$binary", results, "bson type 'binary' should be present in the debug output");
  assert.strContains("$date", results, "bson type 'date' should be present in the debug output");
  assert.strContains("$timestamp", results, "bson type 'timestamp' should be present in the debug output");
  assert.strContains("$regex", results, "bson type 'regex' should be present in the debug output");
  assert.strContains("$oid", results, "bson type 'oid' should be present in the debug output");
  assert.strContains("$undefined", results, "bson type 'undefined' should be present in the debug output");
  assert.strContains("$minKey", results, "bson type 'min' should be present in the debug output");
  assert.strContains("$maxKey", results, "bson type 'max' should be present in the debug output");
  assert.strContains("$numberLong", results, "bson type 'long' should be present in the debug output");
  assert.strContains("$ref", results, "bson type 'dbref' should be present in the debug output");
  assert.strContains("$id", results, "bson type 'dbref' should be present in the debug output");
  assert.strContains("$code", results, "bson type 'javascript' should be present in the debug output");
  assert.strContains("null", results, "bson type 'null' should be present in the debug output");
  assert.strContains("true", results, "bson type 'true' should be present in the debug output");
  assert.strContains("false", results, "bson type 'false' should be present in the debug output");
}());
