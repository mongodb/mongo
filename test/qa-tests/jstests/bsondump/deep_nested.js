// This test checks that bsondump can handle a deeply nested document without breaking

(function(){
  var x = runMongoProgram( "bsondump", "--type=json", "jstests/bsondump/testdata/deep_nested.bson");
  assert.eq(x, 0, "bsondump should exit successfully with 0");
  var x = runMongoProgram( "bsondump", "--type=debug", "jstests/bsondump/testdata/deep_nested.bson");
  assert.eq(x, 2, "bsondump should exit successfully with 0");
})();
