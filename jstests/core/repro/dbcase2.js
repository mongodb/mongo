// SERVER-2111 Check that an in memory db name will block creation of a db with a similar but
// differently cased name.
// @tags: [
//   # The simulate_atlas_proxy override in multi-tenant mode might choose different
//   # prefixes for each sibling DB in this test.
//   multiple_tenants_incompatible,
//   # Can't have 2 databases that just differ on case
//   assumes_no_implicit_collection_creation_on_get_collection,
// ]

// ASCII case pair.
let dbLowerCase = db.getSiblingDB("dbcase2test_dbnamea");
let dbUpperCase = db.getSiblingDB("dbcase2test_dbnameA");

let resultLower = dbLowerCase.c.insert({});
assert.eq(1, resultLower.nInserted);

let resultUpper = dbUpperCase.c.insert({});
assert.commandFailedWithCode(resultUpper, ErrorCodes.DatabaseDifferCase);

assert.eq(-1, db.getMongo().getDBNames().indexOf("dbcase2test_dbnameA"));

// Non-ASCII case pair.
let dbUtf8Lower = db.getSiblingDB("dbcase2test_æbler");
let dbUtf8Upper = db.getSiblingDB("dbcase2test_Æbler");

let resultUtf8Lower = dbUtf8Lower.c.insert({});
assert.eq(1, resultUtf8Lower.nInserted);

let resultUtf8Upper = dbUtf8Upper.c.insert({});
assert.commandFailedWithCode(resultUtf8Upper, ErrorCodes.DatabaseDifferCase);

assert.eq(-1, db.getMongo().getDBNames().indexOf("dbcase2test_Æbler"));
