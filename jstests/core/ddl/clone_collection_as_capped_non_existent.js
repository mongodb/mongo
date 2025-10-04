/**
 * @tags: [
 *   # The test runs commands that are not allowed with security token: CloneCollectionAsCapped,
 *   # cloneCollectionAsCapped.
 *   not_allowed_with_signed_security_token,
 *   requires_non_retryable_commands,
 * ]
 */

// This test ensures that CloneCollectionAsCapped()ing a nonexistent collection will not
// cause the server to abort (SERVER-13750)

let dbname = "clone_collection_as_capped_nonexistent";
let testDb = db.getSiblingDB(dbname);
testDb.dropDatabase();

// Database does not exist here
var res = testDb.runCommand({cloneCollectionAsCapped: "foo", toCollection: "bar", size: 1024});
assert.eq(res.ok, 0, "cloning a nonexistent collection to capped should not have worked");
let isSharded = db.hello().msg == "isdbgrid";

assert.eq(
    res.errmsg,
    isSharded ? "no such cmd: cloneCollectionAsCapped" : "database " + dbname + " not found",
    "converting a nonexistent to capped failed but for the wrong reason",
);

// Database exists, but collection doesn't
testDb.coll.insert({});

var res = testDb.runCommand({cloneCollectionAsCapped: "foo", toCollection: "bar", size: 1024});
assert.eq(res.ok, 0, "cloning a nonexistent collection to capped should not have worked");
assert.eq(
    res.errmsg,
    isSharded ? "no such cmd: cloneCollectionAsCapped" : "source collection " + dbname + ".foo does not exist",
    "converting a nonexistent to capped failed but for the wrong reason",
);
