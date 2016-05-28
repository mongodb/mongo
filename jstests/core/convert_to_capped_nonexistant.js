// This test ensures that ConvertToCapped()ing a nonexistent collection will not cause the server to
// abort (SERVER-13750)

var testDb = db.getSiblingDB("convert_to_capped_nonexistent");
testDb.dropDatabase();

// Database does not exist here
var result = testDb.runCommand({convertToCapped: 'foo', size: 1024});
assert.eq(result.ok, 0, "converting a nonexistent to capped worked and should not have");
assert.eq(
    result.code, 26, "converting a nonexistent database to capped failed, but code has changed");
assert.eq(result.errmsg,
          "database convert_to_capped_nonexistent not found",
          "converting a nonexistent to capped failed, but message has changed");

// Database exists, but collection doesn't
testDb.coll.insert({});

var result = testDb.runCommand({convertToCapped: 'foo', size: 1024});
assert.eq(result.ok, 0, "converting a nonexistent to capped worked and should not have");
assert.eq(
    result.code, 26, "converting a nonexistent collection to capped failed, but code has changed");
assert.eq(result.errmsg,
          "source collection convert_to_capped_nonexistent.foo does not exist",
          "converting a nonexistent to capped failed, but message has changed");
