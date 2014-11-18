// This test makes sure that certain invalid BSON succeeds or fails
// with both JSON and debug output types AND --objcheck

(function(){
  var x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/random_bytes.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given random bytes");
  x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/bad_cstring.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a non-terminated cstring");
  x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/bad_type.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a bad type value");
  x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/partial_file.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given only the start of a file");
  x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/invalid_field_name.bson");
  assert.neq(x, 0, "bsondump should exit with an error given invalid field names");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/random_bytes.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given random bytes");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/bad_cstring.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a non-terminated cstring");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/bad_type.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a bad type value");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/partial_file.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given only the start of a file");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/invalid_field_name.bson");
  assert.neq(x, 0, "bsondump should exit with an error given invalid field names");

  // This should pass, but the content of the output might be erroneous
  x = runMongoProgram( "bsondump", "--objcheck", "jstests/bsondump/testdata/broken_array.bson");
  assert.eq(x, 0, "bsondump should exit with success when given a bad array document");
  x = runMongoProgram( "bsondump", "--objcheck", "--type=debug", "jstests/bsondump/testdata/broken_array.bson");
  assert.eq(x, 0, "bsondump should exit with success when given a bad array document");

  // Make sure recoverable cases do not return an error by default
  clearRawMongoProgramOutput()
  x = runMongoProgram( "bsondump", "jstests/bsondump/testdata/bad_cstring.bson");
  assert.eq(x, 0, "bsondump should not exit with an error when given a non-terminated cstring without --objcheck");
  var results = rawMongoProgramOutput()
  assert.gt(results.search("corrupted"), 0, "one of the documents should have been labelled as corrupted");

})();
