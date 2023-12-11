// Checks that some operations do not create a database
//
// @tags: [
//   assumes_no_implicit_collection_creation_on_get_collection,
//   # The test runs commands that are not allowed with security token: compact.
//   not_allowed_with_signed_security_token,
//   requires_non_retryable_commands,
//   uses_compact
// ]

var adminDB = db.getSiblingDB("admin");
var noDB = function(db) {
    var dbName = db.getName();
    var dbsRes = assert.commandWorked(adminDB.runCommand("listDatabases"));
    dbsRes.databases.forEach(function(e) {
        assert.neq(
            dbName, e.name, "Found db which shouldn't exist:" + dbName + "; " + tojson(dbsRes));
    });
};
var mydb = db.getSiblingDB("neverCreated");
mydb.dropDatabase();
noDB(mydb);

var coll = mydb.fake;

// force:true is for replset passthroughs
assert.commandFailed(coll.runCommand("compact", {force: true}));
noDB(mydb);
assert.commandWorked(coll.insert({}));
mydb.dropDatabase();

assert.commandFailed(coll.runCommand("dropIndexes"));
noDB(mydb);
assert.commandWorked(coll.insert({}));
mydb.dropDatabase();

assert.commandFailed(coll.runCommand("collMod", {expireAfterSeconds: 1}));
noDB(mydb);
assert.commandWorked(coll.insert({}));
mydb.dropDatabase();
