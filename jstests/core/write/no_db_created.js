// Checks that some operations do not create a database
//
// @tags: [
//   assumes_no_implicit_collection_creation_on_get_collection,
//   # The test runs commands that are not allowed with security token: compact.
//   not_allowed_with_signed_security_token,
//   requires_non_retryable_commands,
//   uses_compact
// ]

let adminDB = db.getSiblingDB("admin");
let noDB = function (db) {
    let dbName = db.getName();
    let dbsRes = assert.commandWorked(adminDB.runCommand("listDatabases"));
    dbsRes.databases.forEach(function (e) {
        assert.neq(dbName, e.name, "Found db which shouldn't exist:" + dbName + "; " + tojson(dbsRes));
    });
};
let mydb = db.getSiblingDB("neverCreated");
mydb.dropDatabase();
noDB(mydb);

let coll = mydb.fake;

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
