// This test makes sure that certain invalid BSON succeeds or fails
// with both JSON and debug output types AND --objcheck
(function() {
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/random_bytes.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given random bytes");
  x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/bad_cstring.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a non-terminated cstring");
  x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/bad_type.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a bad type value");
  x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/partial_file.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given only the start of a file");
  x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/invalid_field_name.bson");
  assert.neq(x, 0, "bsondump should exit with an error given invalid field names");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/random_bytes.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given random bytes");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/bad_cstring.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a non-terminated cstring");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/bad_type.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given a bad type value");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/partial_file.bson");
  assert.neq(x, 0, "bsondump should exit with an error when given only the start of a file");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/invalid_field_name.bson");
  assert.neq(x, 0, "bsondump should exit with an error given invalid field names");

  // This should pass, but the content of the output might be erroneous
  x = _runMongoProgram("bsondump", "--objcheck", "jstests/bson/testdata/broken_array.bson");
  assert.eq(x, 0, "bsondump should exit with success when given a bad array document");
  x = _runMongoProgram("bsondump", "--objcheck", "--type=debug", "jstests/bson/testdata/broken_array.bson");
  assert.eq(x, 0, "bsondump should exit with success when given a bad array document");

  // Make sure recoverable cases do not return an error by default
  clearRawMongoProgramOutput();
  x = _runMongoProgram("bsondump", "jstests/bson/testdata/bad_cstring.bson");
  assert.eq(x, 0, "bsondump should not exit with an error when given a non-terminated cstring without --objcheck");
  assert.strContains.soon("corrupted", rawMongoProgramOutput,
      "one of the documents should have been labelled as corrupted");

}());
