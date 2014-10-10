// This test ensures that ConvertToCapped()ing a nonexistent collection will not cause the server to
// abort (SERVER-13750)

var testDb = db.getSiblingDB("convert_to_capped_nonexistent");
testDb.dropDatabase();
var result = testDb.runCommand({convertToCapped: 'foo', size: 1024});
assert.eq(result.ok, 0, "converting a nonexistent to capped worked and should not have");
assert.eq(result.errmsg, "source collection convert_to_capped_nonexistent.foo does not exist",
          "converting a nonexistent to capped failed but for the wrong reason");
