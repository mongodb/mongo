// This test runs bsondump on a .bson file containing non-deprecated BSON types 
// and makes sure their JSON type representations exist in the output.

(function(){
  var x = _runMongoProgram( "bsondump", "--type=json", "jstests/bson/testdata/all_types.bson");
  assert.eq(x, 0, "bsondump should exit successfully with 0");

  sleep(100);

  var results = rawMongoProgramOutput();
  assert.gt(results.search("\\$binary"), 0, "bson type 'binary' should be present in the debug output");
  assert.gt(results.search("\\$date"), 0, "bson type 'date' should be present in the debug output");
  assert.gt(results.search("\\$timestamp"), 0, "bson type 'timestamp' should be present in the debug output");
  assert.gt(results.search("\\$regex"), 0, "bson type 'regex' should be present in the debug output");
  assert.gt(results.search("\\$oid"), 0, "bson type 'oid' should be present in the debug output");
  assert.gt(results.search("\\$undefined"), 0, "bson type 'undefined' should be present in the debug output");
  assert.gt(results.search("\\$minKey"), 0, "bson type 'min' should be present in the debug output");
  assert.gt(results.search("\\$maxKey"), 0, "bson type 'max' should be present in the debug output");
  assert.gt(results.search("\\$numberLong"), 0, "bson type 'long' should be present in the debug output");
  assert.gt(results.search("\\$ref"), 0, "bson type 'dbref' should be present in the debug output");
  assert.gt(results.search("\\$id"), 0, "bson type 'dbref' should be present in the debug output");
  assert.gt(results.search("\\$code"), 0, "bson type 'javascript' should be present in the debug output");
  assert.gt(results.search("null"), 0, "bson type 'null' should be present in the debug output");
  assert.gt(results.search("true"), 0, "bson type 'true' should be present in the debug output");
  assert.gt(results.search("false"), 0, "bson type 'false' should be present in the debug output");
})();
