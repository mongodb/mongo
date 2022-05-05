// SERVER-2111 Check that an in memory db name will block creation of a db with a similar but
// differently cased name.
// @tags: [
//   # The inject_tenant_prefix override in shard split mode might choose different
//   # prefixes for each sibling DB in this test.
//   shard_split_incompatible,
// ]

var dbLowerCase = db.getSiblingDB("dbcase2test_dbnamea");
var dbUpperCase = db.getSiblingDB("dbcase2test_dbnameA");

var resultLower = dbLowerCase.c.insert({});
assert.eq(1, resultLower.nInserted);

var resultUpper = dbUpperCase.c.insert({});
assert.commandFailed(resultUpper);

assert.eq(-1, db.getMongo().getDBNames().indexOf("dbcase2test_dbnameA"));
